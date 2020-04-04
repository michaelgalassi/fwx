/*
 * FreeWX - logger for Davis weather stations
 *
 * Copyright 2003-2019 Michael Galassi, all rights reserved.
 *
 * This code may be re-distributed under the terms and conditions of
 * the BSD 2 clause license.
 *
 * This code was once loosely based on Alan Batie's wiz3d.
 *
 * All questions & comments should be directed to me at michael at
 * galassi dot us.
 */

#include <sys/types.h>
#include <sys/uio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/rtprio.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "fwx.h"
#include "davis.h"

#define VERSION_MAJ 0
#define VERSION_MIN 5
#define CONFIG "/usr/local/etc/fwx.conf"

#define USAGE "usage:\n%s [-b] [-i <interval>] -l <logdir> -d <device>\n"

/* from crc.c */
extern int wxcrc(unsigned char *buf, int len);
/* from support.c */
extern void dumpbuf(FILE *stream, unsigned char *p, size_t len);
extern int wxopen(char *wxdev);
extern int wxread(int wxfd, void *buf, size_t len, int timeout);
extern int wxwakeup(int wxfd);
extern int wxcmd(int fd, char *cmd);
extern int wxgetack(int fd);
/* forward declarations from this file */
static int wxident(int fd);
static void wxlog(char *wxlogdir, wxdat_t *wxdat);
static void wxgetloop(int fd, wxdat_t *wxdat);
static void wxsendwu(wxdat_t *wxdp);
static void wxsendcwop(wxdat_t *wxdp);
static void wxsendaeris(wxdat_t *wxdp);

static char fwxdev[64];
static char fwxlogdir[64];
static char wustation[64];
static char wupassword[64];
static char aerisstation[64];
static char aerispassword[64];
static char cwopsvr[64];
static char cwopuser[64];
static char cwoploc[64];
static int fwxinterval = 30;     /* default to sampling every 30 sec */

static void
alarmcatcher(int sig)
{
    (void)sig;
    return;
}

/*
 * tease apart a line from the config file, the lines of interest
 * have a name and a value separated by white space, eg:
 * WXLOGDIR /var/fwx
 * lines that don't make sense are quietly ignored
 */
static int
chkvar(const char *s, const char *name, char *dst, size_t len)
{
    size_t l;
    char *p;

    l = strlen(name);
    if (strncmp(s, name, l) != 0) {
        return 0;       /* bug out if this isn't the line of interest */
    }
    s += l;             /* point past variable name */
    if (!*s) {
        return 0;
    }
    while (isspace(*s)) {
        ++s;            /* loose white space */
    }
    if (!*s) {
        return 0;
    }
    strncpy(dst, s, len);
    p = &dst[len-1];
    /* loose trailing newline and any other white space */
    while (!*p || isspace(*p)) {
        *p-- = '\0';
    }
    return 1;
}

int
main(int argc, char **argv)
{
    wxdat_t wxdat;
    struct itimerval itv;
    struct rtprio rtp;
    int wxfd;
    int c;
    struct stat st;
    int background;           /* foreground by default */
    char str[128];
    char tmpstr[8];
    char *s;
    FILE *fp;

    /*
     * read the optional config file first so command line
     * arguments can override it
     */
    if ((fp = fopen(CONFIG, "r")) != (FILE *)0) {
        while(fgets(str, sizeof(str), fp) != (char *)0) {
            s = str;
            while (isspace(*s)) {
                ++s;            /* clean up leading cruft */
            }
            if (chkvar(s, "FWXLOGDIR", fwxlogdir, sizeof(fwxlogdir)-1)) {
                continue;
            }
            if (chkvar(s, "FWXDEV", fwxdev, sizeof(fwxdev)-1)) {
                continue;
            }
            if (chkvar(s, "FWXINTERVAL", tmpstr, sizeof(tmpstr)-1)) {
                fwxinterval = (int)strtol(tmpstr, (char **)0, 0);
                continue;
            }
            if (chkvar(s, "WUSTATION", wustation, sizeof(wustation)-1)) {
                continue;
            }
            if (chkvar(s, "WUPASSWORD", wupassword, sizeof(wupassword)-1)) {
                continue;
            }
            if (chkvar(s, "AERISSTATION", aerisstation, sizeof(aerisstation)-1)) {
                continue;
            }
            if (chkvar(s, "AERISPASSWORD", aerispassword, sizeof(aerispassword)-1)) {
                continue;
            }
            if (chkvar(s, "CWOPSVR", cwopsvr, sizeof(cwopsvr)-1)) {
                continue;
            }
            if (chkvar(s, "CWOPUSER", cwopuser, sizeof(cwopuser)-1)) {
                continue;
            }
            if (chkvar(s, "CWOPLOC", cwoploc, sizeof(cwoploc)-1)) {
                continue;
            }
            /* ignore any line that doesn't match */
        }
        fclose(fp);
    }

    background = 0;
    while ((c = getopt(argc, argv, "d:i:l:b")) != -1) {
        switch (c) {
        case 'd':
            strncpy(fwxdev, optarg, sizeof(fwxdev)-1);
            break;
        case 'i':
            fwxinterval = (int)strtol(optarg, (char **)0, 0);
            break;
        case 'l':
            strncpy(fwxlogdir, optarg, sizeof(fwxlogdir)-1);
            break;
        case 'b':
           ++background;
           break;
        default:
            fprintf(stderr, USAGE, argv[0]);
            return 1;
        }
    }
#ifdef DEBUG_CONFIG
    fprintf(stdout, "station %s password %s logdir %s dev %s interval %d\n",
            wustation, wupassword, fwxlogdir, fwxdev, fwxinterval);
#endif /*DEBUG_CONFIG*/

    /*
     * these two have no default and neither one is optional
     */
    if (!*fwxdev || !*fwxlogdir) {
        fprintf(stderr, USAGE, argv[0]);
        return 1;
    }
    /*
     * open and configure the serial device to a state the station can
     * talk to.
     */
    if ((wxfd = wxopen(fwxdev)) == -1) {
        fprintf(stderr, "wxopen failed for device %s\n", fwxdev);
        return 1;
    }

    /*
     * check that wxlogdir exists & is a directory, all other checks
     * will wait 'til we try to log into it
     */
    if ((stat(fwxlogdir, &st)) == -1) {
        fprintf(stderr, "cannot access log director %s\n", fwxlogdir);
        return 1;
    }
    if ((st.st_mode & S_IFDIR) == 0) {
        fprintf(stderr, "%s is not a directory\n", fwxlogdir);
        return 1;
    }

    switch (wxident(wxfd)) {
    case IDENT_VP:      /* vantage pro or vantage pro2 */
        break;
    default:
        fprintf(stderr, "Only Vantage Pro and Pro2 are supported\n");
        return 1;
    }

    if (background && daemon(0, 0) == -1) {
        perror("daemon");
        return 1;
    }

    rtp.type = RTP_PRIO_REALTIME;
    rtp.prio = 16;
    if ((rtprio(RTP_SET, 0, &rtp)) != 0) {
        perror("rtprio");
    }

    signal(SIGALRM, alarmcatcher);
    itv.it_interval.tv_sec = itv.it_value.tv_sec = fwxinterval;
    itv.it_interval.tv_usec = itv.it_value.tv_usec = 0;
    (void)setitimer(ITIMER_REAL, &itv, (struct itimerval *)0);
    while (1) {
        memset((void *)&wxdat, 0, sizeof(wxdat_t));
        wxdat.time = time((time_t *)0);
        wxgetloop(wxfd, &wxdat);
        wxlog(fwxlogdir, &wxdat);
        wxsendwu(&wxdat);
        wxsendcwop(&wxdat);
        wxsendaeris(&wxdat);
        /* wait for my itimer to expire */
        (void)select(0, (fd_set *)0, (fd_set *)0, (fd_set *)0, (struct timeval *)0);
    }
    return 0;   /*NOTREACHED*/
}

static void
wxlog(char *wxlogdir, wxdat_t *wxdp)
{
    char str[FILENAME_MAX];
    char *s;
    FILE *file;

    (void)strcpy(str, wxlogdir);
    s = &str[strlen(str)];
    if (*s == '/') {
        strftime(s, 15, "%Y.%m.%d.fwx", localtime(&wxdp->time));
    } else {
        strftime(s, 16,"/%Y.%m.%d.fwx", localtime(&wxdp->time));
    }
    if ((file = fopen(str, "a+")) == (FILE *)0) {
        perror("wxlog - fopen");
        return;
    }

    s = str;            /* reuse the same buffer */

    s += sprintf(s, "%d,%d,%ld,", VERSION_MAJ, VERSION_MIN, (long int)wxdp->time);

    if (WXD_ISVALID(wxdp->barometer)) {
        s += sprintf(s, "%.3f,", WXD_GETDAT(wxdp->barometer, floatd));
    } else {
        *s++ = ','; *s++ = '\0';
    }
    if (WXD_ISVALID(wxdp->windspeed)) {
        s += sprintf(s, "%.0f,", WXD_GETDAT(wxdp->windspeed, floatd));
        if (WXD_ISVALID(wxdp->winddir)) {
            s += sprintf(s, "%.0f,", WXD_GETDAT(wxdp->winddir, floatd));
        } else {
            *s++ = ','; *s++ = '\0';
        }
    } else {
        *s++ = ','; *s++ = ','; *s++ = '\0';
    }
    if (WXD_ISVALID(wxdp->avgwindspeed)) {
        s += sprintf(s, "%.0f,", WXD_GETDAT(wxdp->avgwindspeed, floatd));
    } else {
        *s++ = ','; *s++ = '\0';
    }
    if (WXD_ISVALID(wxdp->indoortemp)) {
        s += sprintf(s, "%.1f,", WXD_GETDAT(wxdp->indoortemp, floatd));
    } else {
        *s++ = ','; *s++ = '\0';
    }
    if (WXD_ISVALID(wxdp->outdoortemp)) {
        s += sprintf(s, "%.1f,", WXD_GETDAT(wxdp->outdoortemp, floatd));
    } else {
        *s++ = ','; *s++ = '\0';
    }
    if (WXD_ISVALID(wxdp->outdoordewpoint)) {
        s += sprintf(s, "%.1f,", WXD_GETDAT(wxdp->outdoordewpoint, floatd));
    } else {
        *s++ = ','; *s++ = '\0';
    }
    if (WXD_ISVALID(wxdp->indoorhum)) {
        s += sprintf(s, "%.0f,", WXD_GETDAT(wxdp->indoorhum, floatd));
    } else {
        *s++ = ','; *s++ = '\0';
    }
    if (WXD_ISVALID(wxdp->outdoorhum)) {
        s += sprintf(s, "%.0f,", WXD_GETDAT(wxdp->outdoorhum, floatd));
    } else {
        *s++ = ','; *s++ = '\0';
    }
    if (WXD_ISVALID(wxdp->rainrate)) {
        s += sprintf(s, "%.2f,", WXD_GETDAT(wxdp->rainrate, floatd));
    } else {
        *s++ = ','; *s++ = '\0';
    }
#ifdef NOTYET
    if (WXD_ISVALID(wxdp->rainhour)) {
        s += sprintf(s, "%.2f,", WXD_GETDAT(wxdp->rainhour, floatd));
    } else {
        *s++ = ','; *s++ = '\0';
    }
#endif /*NOTYET*/
    if (WXD_ISVALID(wxdp->rainday)) {
        s += sprintf(s, "%.2f,", WXD_GETDAT(wxdp->rainday, floatd));
    } else {
        *s++ = ','; *s++ = '\0';
    }
    if (WXD_ISVALID(wxdp->rainmonth)) {
        s += sprintf(s, "%.2f,", WXD_GETDAT(wxdp->rainmonth, floatd));
    } else {
        *s++ = ','; *s++ = '\0';
    }
    if (WXD_ISVALID(wxdp->rainyear)) {
        s += sprintf(s, "%.2f,", WXD_GETDAT(wxdp->rainyear, floatd));
    } else {
        *s++ = ','; *s++ = '\0';
    }
    if (WXD_ISVALID(wxdp->solar)) {
        s += sprintf(s, "%d,", WXD_GETDAT(wxdp->solar, intd));
    } else {
        *s++ = ','; *s++ = '\0';
    }

    (void)fprintf(file, "%s\n", str);
    if (fclose(file) == EOF) {
        perror("wxlog - fclose");
        return;
    }

    return;
}

static void
wxcalcdewpoint(wxdat_t *wxdp)
{
    float dewpoint;
    float temp;
    float hum;
    float ews;
    char units;

    if (!WXD_ISVALID(wxdp->outdoortemp) || !WXD_ISVALID(wxdp->outdoorhum)) {
        return;
    }
    temp = WXD_GETDAT(wxdp->outdoortemp, floatd);
    hum = WXD_GETDAT(wxdp->outdoorhum, floatd);

    if (WXD_GETFLAGS(wxdp->outdoorhum) & WXD_ENGLISH) {
        units = 'F';
        /* convert to celcius */
        temp = (temp - 32.0) * 5.0 / 9.0;
    } else {
        units = 'C';
    }
#if 0           /* don't remember where I got this */
    if ((ews = hum * 0.01 * exp((17.502 * temp)/(240.9 + temp))) < 0) {
        return;
    }
    dewpoint = (240.9 * log(ews))/(17.502 - log(ews));
#else
    /*
     * From app note AN_28 from Davis, the numbers reflect Sonntag's
     * paper from 1990.
     */
    ews = hum * 0.01 * 6.112 * exp((17.62 * temp)/(243.12 + temp));
    dewpoint = (243.12 * log(ews) - 440.1 ) / (19.43 - log(ews));
#endif
    if (isnan(dewpoint)) {
        return;
    }
    if (units == 'F') {
        /* convert back to farenheight */
        dewpoint = (dewpoint * 9.0) / 5.0 + 32.0;
        WXD_SETDAT(wxdp->outdoordewpoint, dewpoint, floatd);
        WXD_SETUNITS(wxdp->outdoordewpoint, "deg F");
        WXD_SETFLAGS(wxdp->outdoordewpoint, WXD_VALID|WXD_ENGLISH|1);
    } else {
        WXD_SETDAT(wxdp->outdoordewpoint, dewpoint, floatd);
        WXD_SETUNITS(wxdp->outdoordewpoint, "deg C");
        WXD_SETFLAGS(wxdp->outdoordewpoint, WXD_VALID|WXD_METRIC|1);
    }
}

static const wind_t *
wxcalcwindgust(wind_t *wp)
{
    static wind_t *warr;
    static int walen;
    static int waidx;
    static int tenmin;
    int found;
    int gust;
    int idx;
    int i;
    int f;

    /* first time around allocate storage and initialize "stuff" */
    if (!walen) {
        i = tenmin = 10 * 60 / fwxinterval + 1;
        walen = 1;
        do {
            walen <<= 1;
        } while (i >>= 1);
        if (!(warr = malloc(walen * sizeof(wind_t)))) {
            walen = 0;
            return (wind_t *)0;
        }
        memset((void *)warr, 0, walen * sizeof(wind_t));
    }
    /* save new wind point */
    memcpy(&warr[waidx], wp, sizeof(wind_t));
    waidx = (waidx + 1) & (walen - 1);
    /* find highest value in the last 10 minutes */
    f = (waidx + (walen - tenmin)) & (walen - 1);
    gust = 0;
    for (i = 0; i < tenmin; ++i) {
        idx = (f + i) & (walen - 1);
        if (warr[idx].speed > gust) {
            gust = warr[idx].speed;
            found = idx;
        }
    }
    return &warr[found];
}

#ifdef NOTYET
static void
wxcalcrainhour(wxdat_t *wxdatp)
{
    struct tm *tmp;
    int i;
    float in;

    static struct {
        time_t time;    /* time of sample described */
        float inches;   /* rain inches in last 60 1-minute intervals */
    } rainhour[60];
    static int inited;


    /* initialize array first time through */
    if (!inited) {
        for (i = 0; i < 60; ++i) {
            rainhour[i].time = wxdatp->time;
            rainhour[i].inches = WXD_GETDAT(wxdatp->rainyear, floatd);
        }
        inited = 1;
    }

    /* if we don't have a valid rain reading just return */
    if (!WXD_ISVALID(wxdatp->rainyear)) {
            WXD_SETUNITS(wxdatp->rainhour, "in");
            WXD_SETFLAGS(wxdatp->rainhour, WXD_INVALID);
    }

    tmp = localtime(wxdatp->time);
    i = tmp->tm_min;            /* use minutes as index */
    in = rainhour[i];
    rainhour[i] = WXD_GETDAT(wxdatp->rainyear, floatd);
    WXD_SETDAT(wxdatp->rainhour, rainhour[i] - in, floatd);
    WXD_SETFLAGS(wxdatp->rainhour, WXD_VALID|WXD_ENGLISH|1);
}
#endif /*NOTYET*/

#define EIGHT_ONES 0xff
#define SIXTEEN_ONES 0xffff

static void
cvtvploop2fwx(vploopdata_t *ld, wxdat_t *wxdatp)
{
    const wind_t *wg;
    unsigned int tmp;
    int stmp;

    WXD_SETUNITS(wxdatp->barometer, "in");
    tmp = get_d_16(ld->bar);
    if (tmp != SIXTEEN_ONES) {
        WXD_SETDAT(wxdatp->barometer, (float)tmp / 1000, floatd);
        WXD_SETFLAGS(wxdatp->barometer, WXD_VALID|WXD_ENGLISH|3);
    }
    if ((tmp = get_d_8(ld->windSpeed)) != EIGHT_ONES) {
        wxdatp->windcur.speed = tmp;
    }
    if ((tmp = get_d_16(ld->windDir)) <= 360) {
        wxdatp->windcur.direction = tmp;
    }
    if ((tmp = get_d_8(ld->windSpeed10)) != EIGHT_ONES) {
        wxdatp->windavg.speed = tmp;
    }
    if ((wg = wxcalcwindgust(&wxdatp->windcur))) {
        memcpy(&wxdatp->windgust, wg, sizeof(wind_t));
    }
    WXD_SETUNITS(wxdatp->windspeed, "mph");
    tmp = get_d_8(ld->windSpeed);
    if (tmp != EIGHT_ONES) {
        WXD_SETDAT(wxdatp->windspeed, (float)tmp, floatd);
        WXD_SETFLAGS(wxdatp->windspeed, WXD_VALID|WXD_ENGLISH|0);
    }
    WXD_SETUNITS(wxdatp->winddir, "deg");
    tmp = get_d_16(ld->windDir);
    if (tmp <= 360) {
        WXD_SETDAT(wxdatp->winddir, (float)tmp, floatd);
        WXD_SETFLAGS(wxdatp->winddir, WXD_VALID|0);
    }
    WXD_SETUNITS(wxdatp->avgwindspeed, "mph");
    WXD_SETUNITS(wxdatp->avgwindspeedinterval, "min");
    tmp = get_d_8(ld->windSpeed10);
    if (tmp != EIGHT_ONES) {
        WXD_SETDAT(wxdatp->avgwindspeed, (float)tmp, floatd);
        WXD_SETFLAGS(wxdatp->avgwindspeed, WXD_VALID|WXD_ENGLISH|0);
        WXD_SETDAT(wxdatp->avgwindspeedinterval, (float)10, floatd);
        WXD_SETFLAGS(wxdatp->avgwindspeedinterval, WXD_VALID|0);
    }
    WXD_SETUNITS(wxdatp->indoortemp, "deg F");
    stmp = get_d_16(ld->tempIn);
    if (stmp != 0x1000 && stmp > -1500 && stmp < 1500) {
        WXD_SETDAT(wxdatp->indoortemp, (float)stmp / 10, floatd);
        WXD_SETFLAGS(wxdatp->indoortemp, WXD_VALID|WXD_ENGLISH|1);
    }
    WXD_SETUNITS(wxdatp->outdoortemp, "deg F");
    stmp = get_d_16(ld->tempOut);
    if (stmp != 0x1000 && stmp > -1500 && stmp < 1500) {
        WXD_SETDAT(wxdatp->outdoortemp, (float)stmp / 10, floatd);
        WXD_SETFLAGS(wxdatp->outdoortemp, WXD_VALID|WXD_ENGLISH|1);
    }
    WXD_SETUNITS(wxdatp->indoorhum, "%");
    tmp = get_d_8(ld->humIn);
    if (tmp != EIGHT_ONES && tmp <= 100) {
        WXD_SETDAT(wxdatp->indoorhum, (float)tmp, floatd);
        WXD_SETFLAGS(wxdatp->indoorhum, WXD_VALID|0);
    }
    WXD_SETUNITS(wxdatp->outdoorhum, "%");
    tmp = get_d_8(ld->humOut);
    if (tmp != EIGHT_ONES && tmp <= 100) {
        WXD_SETDAT(wxdatp->outdoorhum, (float)tmp, floatd);
        WXD_SETFLAGS(wxdatp->outdoorhum, WXD_VALID|0);
    }
    WXD_SETUNITS(wxdatp->rainrate, "in/hr");
    tmp = get_d_16(ld->rainRate);
    if (tmp != SIXTEEN_ONES) {
        WXD_SETDAT(wxdatp->rainrate, (float)tmp / 100, floatd);
        WXD_SETFLAGS(wxdatp->rainrate, WXD_VALID|WXD_ENGLISH|2);
    }
    WXD_SETUNITS(wxdatp->solar, "w/m2");
    tmp = get_d_16(ld->solarRad);
    if (tmp != SIXTEEN_ONES) {
        WXD_SETDAT(wxdatp->solar, tmp, intd);
        /* w/m2 sounds metric... */
        WXD_SETFLAGS(wxdatp->solar, WXD_VALID|WXD_METRIC|2);
    }
    WXD_SETUNITS(wxdatp->rainday, "in");
    tmp =  get_d_16(ld->rainDay);
    if (tmp != SIXTEEN_ONES) {
        WXD_SETDAT(wxdatp->rainday, (float)tmp / 100, floatd);
        WXD_SETFLAGS(wxdatp->rainday, WXD_VALID|WXD_ENGLISH|2);
    }
    WXD_SETUNITS(wxdatp->rainmonth, "in");
    tmp = get_d_16(ld->rainMonth);
    if (tmp != SIXTEEN_ONES) {
        WXD_SETDAT(wxdatp->rainmonth, (float)tmp / 100, floatd);
        WXD_SETFLAGS(wxdatp->rainmonth, WXD_VALID|WXD_ENGLISH|2);
    }
    WXD_SETUNITS(wxdatp->rainyear, "in");
    tmp = get_d_16(ld->rainYear);
    if (tmp != SIXTEEN_ONES) {
        WXD_SETDAT(wxdatp->rainyear, (float)tmp / 100, floatd);
        WXD_SETFLAGS(wxdatp->rainyear, WXD_VALID|WXD_ENGLISH|2);
    }
    wxcalcdewpoint(wxdatp);     /* figure out dewpoint */
#ifdef NOTYET
    wxcalcrainhour(wxdatp);     /* figure out rain in last 60 minutes */
#endif /*NOTYET*/

    return;
}

static void
wxgetloop(int fd, wxdat_t *wxdatp)
{
    vploopdata_t ld;
    int i;
    int rc;

    for (i = 0; i < 4; ++i) {
#ifdef DEBUG_WXLOOP
        fprintf(stdout, "wxgetloop - wakeup attempt %d\n", i);
#endif /*DEBUG_WXLOOP*/
        if (wxwakeup(fd) == 0) {
            break;
        }
    }

    if (wxcmd(fd, VPLOOPCMD) != 0) {
        return;
    }

    if ((rc = wxread(fd, (void *)&ld, sizeof(vploopdata_t), 10)) == -1) {
        fprintf(stderr, "wxgetloop() wxread failed\n");
        return;
    }

    if (rc != sizeof(vploopdata_t)) {
        fprintf(stderr, "wxgetloop - got %d bytes, expected %zu\n",
                rc, sizeof(vploopdata_t));
#ifdef DEBUG_WXLOOP
        dumpbuf(stdout, (unsigned char *)&ld, rc);
#endif /*DEBUG_WXLOOP*/
        return;
    }

    if (wxcrc((unsigned char *)&ld + 1, rc) != 0) {
        fprintf(stderr, "wxgetloop - got bogus crc\n");
#ifdef DEBUG_WXLOOP
        dumpbuf(stdout, (unsigned char *)&ld, rc);
#endif /*DEBUG_WXLOOP*/
        return;
    }

    cvtvploop2fwx(&ld, wxdatp);
}

/*
 * https://feedback.weather.com/customer/en/portal/articles/2924682-pws-upload-protocol?b_id=17298
 */
static void
wxsendwu(wxdat_t *wxdp)
{
    struct tm *tm;
    char str[2048];
    char *s;

    if (!*wustation || !*wupassword) {
        /* if we have no station or password we just log to our CSV file */
        return;
    }
    s = stpcpy(str, "/usr/bin/fetch -q -a -T 3 -o /dev/null 'http://rtupdate.wunderground.com/weatherstation/updateweatherstation.php?action=updateraw&realtime=1");
    s += sprintf(s, "&rtfreq=%d", fwxinterval);
    s += sprintf(s, "&ID=%s&PASSWORD=%s", wustation, wupassword);
    s = stpcpy(s, "&dateutc=");
    tm = gmtime(&wxdp->time);
    s += strftime(s, 32, "%Y-%m-%d%%20%H%%3A%M%%3A%S", tm);
    s += sprintf(s, "&softwaretype=fwx%%20v%d.%d", VERSION_MAJ, VERSION_MIN);
    s += sprintf(s, "&windspeedmph=%d", wxdp->windcur.speed);
    if (wxdp->windcur.speed != 0) {
        s += sprintf(s, "&winddir=%d", wxdp->windcur.direction);
    }
    s += sprintf(s, "&windgustmph=%d", wxdp->windgust.speed);
    if (wxdp->windgust.speed != 0) {
        s += sprintf(s, "&windgustdir=%d", wxdp->windgust.direction);
    }
    if (WXD_ISVALID(wxdp->outdoortemp)) {
        s += sprintf(s, "&tempf=%.1f", WXD_GETDAT(wxdp->outdoortemp, floatd));
    }
    if (WXD_ISVALID(wxdp->rainrate)) {
        s += sprintf(s, "&rainin=%.2f", WXD_GETDAT(wxdp->rainrate, floatd));
    }
    if (WXD_ISVALID(wxdp->rainday)) {
        s += sprintf(s, "&dailyrainin=%.2f", WXD_GETDAT(wxdp->rainday, floatd));
    }
    if (WXD_ISVALID(wxdp->barometer)) {
        s += sprintf(s, "&baromin=%.3f", WXD_GETDAT(wxdp->barometer, floatd));
    }
    if (WXD_ISVALID(wxdp->outdoorhum)) {
        s += sprintf(s, "&humidity=%.0f", WXD_GETDAT(wxdp->outdoorhum, floatd));
    }
    if (WXD_ISVALID(wxdp->outdoordewpoint)) {
        s += sprintf(s, "&dewptf=%.1f", WXD_GETDAT(wxdp->outdoordewpoint, floatd));
    }
    if (WXD_ISVALID(wxdp->solar)) {
        s += sprintf(s, "&solarradiation=%d", WXD_GETDAT(wxdp->solar, intd));
    }
    *s++ = '\''; *s = '\0';
    (void)system(str);
}

static void
waitforsrv(int s)
{
    fd_set rfds;
    int rc;
    char str[512];

    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    switch (rc = select(s + 1, &rfds, (fd_set *)0, (fd_set *)0, (struct timeval *)0)) {
    case 0:
	printf("select returned %d???\n", rc);
	break;
    case -1:
	perror("select");
	break;
    case 1:
	rc = read(s, str, sizeof(str));
#ifdef DEBUG_CWOP
        str[rc] = '\0';
	printf("got \"%s\"\n", str);
#endif /*DEBUG_CWOP*/
	break;
    default:
	printf("select returned %d???\n", rc);
	break;
    }
}

/*
 * http://www.wxqa.com/faq.html
 */
static void
wxsendcwop(wxdat_t *wxdp)
{
    static time_t lasthere;
    time_t now;
    struct sockaddr_in sin;
    struct hostent *hep;
    struct tm *tm;
    int tmp;
    int s;
    int len;
    char str[256];
    char *sp;

    if (!*cwopsvr || !*cwopuser || !*cwoploc) {
        /* don't bother if we don't have the server, login, and location */ 
#ifdef DEBUG_CWOP
        printf("not logging to CWOP svr: %s user: %s location: %s\n",
               cwopsvr, cwopuser, cwoploc);
#endif
        return;
    }
    now = time((time_t *)0);
    if (now - lasthere < 5 * 60) {
        /* don't do this more than every 5 minutes */
        return;
    }
    /* open connection to port 14580 on the server */
    memset((void *)&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(14580);
    if ((hep = gethostbyname(cwopsvr)) != (struct hostent *)0) {
        in_addr_t *in = (in_addr_t *)hep->h_addr_list[0];
        sin.sin_addr.s_addr = *in;
    } else {
        sin.sin_addr.s_addr = inet_addr(cwopsvr);
    }
    if (sin.sin_addr.s_addr == INADDR_NONE) {
#ifdef DEBUG_CWOP
        printf("can't figure out the server address\n");
#endif /*DEBUG_CWOP*/
        return;
    }
#ifdef DEBUG_CWOP
        printf("CWOP server address 0x%08x\n", sin.sin_addr.s_addr);
#endif /*DEBUG_CWOP*/
    if ((s = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
#ifdef DEBUG_CWOP
        perror("socket");
#endif /*DEBUG_CWOP*/
        return;
    }
    if (connect(s, (const struct sockaddr *)&sin, sizeof(sin)) == -1) {
        perror("connect");
        (void)close(s);
        return;
    }
    /* think about blocking vs non-blocking so we can read what we're
     * getting back for debug purposes
     */
    waitforsrv(s);
    /* "login" by sending user, passcode, and software id */
    if ((len = snprintf(str, sizeof(str), "user %s pass -1 vers fwx %d.%d\r\n",
                        cwopuser, VERSION_MAJ, VERSION_MIN)) < 0) {
        return;
    }
    if (write(s, str, len) == -1) {
	perror("write login");
        (void)close(s);
        return;
    }
    /* build up packet */
    sp = stpcpy(str, cwopuser);
    tm = gmtime(&wxdp->time);
    sp += strftime(sp, 32, ">APRS,TCPIP*:@%d%H%M", tm);
    sp += sprintf(sp, "z%s", cwoploc);
    sp += sprintf(sp, "_%03d/%03dg%03d", wxdp->windcur.direction,
		  wxdp->windcur.speed, wxdp->windgust.speed);
    tmp = (int)nearbyintf(WXD_GETDAT(wxdp->outdoortemp, floatd));
    if (tmp < 0) {
        sp += sprintf(sp, "t-%02d", -tmp);
    } else {
        sp += sprintf(sp, "t%03d", tmp);
    }
#ifdef NOTYET
    sp += sprintf(sp, "r%03d", wxdp->rainhour);
#endif /*NOTYET*/
    /* until I get these figured out */
    sp += sprintf(sp, "r...p...");
    /* rain today in one hundredths of an inch */
    tmp = (int)nearbyintf(WXD_GETDAT(wxdp->rainday, floatd)*100);
    sp += sprintf(sp, "P%03d", tmp);
    /* humidity in percent, 2 digits, 100 is special cased as 00 */
    tmp = (int)nearbyintf(WXD_GETDAT(wxdp->outdoorhum, floatd));
    sp += sprintf(sp, "h%02d", tmp > 99 ? 0 : tmp);
    /* pressure is in millibar * 10 rather than in of Hg */
    tmp = (int)nearbyintf(WXD_GETDAT(wxdp->barometer, floatd) * 33.86389 * 10);
    sp += sprintf(sp, "b%05d", tmp);
    /* solar radiation in w/m^2 */
    if ((tmp = WXD_GETDAT(wxdp->solar, intd)) > 999) {
        sp += sprintf(sp, "l%03d", tmp - 1000);
    } else {
        sp += sprintf(sp, "L%03d", tmp);
    }
    /* fwx software */
    sp += sprintf(sp, "wfwx\r\n");
    /* when server's ready */
    waitforsrv(s);
    /* send packet */
#ifdef DEBUG_CWOP
    printf("\"%s\"\n", str);
#endif /*DEBUG_CWOP*/
    if (write(s, str, strlen(str)) == -1) {
        perror("write packet");
        (void)close(s);
        return;
    }
    /* close connection */
    (void)close(s);
    lasthere = now;
    return;
}

static void
wxsendaeris(wxdat_t *wxdp)
{
    struct tm *tm;
    char str[2048];
    char *s;

    if (!*aerisstation || !*aerispassword) {
        /* if we have no station or password we just log to our CSV file */
        return;
    }
    s = stpcpy(str, "/usr/bin/fetch -q -a -T 3 -o /dev/null 'https://www.pwsweather.com/pwsupdate/pwsupdate.php?");
    s += sprintf(s, "ID=%s&PASSWORD=%s", aerisstation, aerispassword);
    s = stpcpy(s, "&dateutc=");
    tm = gmtime(&wxdp->time);
    s += strftime(s, 32, "%Y-%m-%d+%H%%3A%M%%3A%S", tm);
    s += sprintf(s, "&windspeedmph=%d", wxdp->windcur.speed);
    if (wxdp->windcur.speed != 0) {
        s += sprintf(s, "&winddir=%d", wxdp->windcur.direction);
    }
    s += sprintf(s, "&windgustmph=%d", wxdp->windgust.speed);
#if 0
    if (wxdp->windgust.speed != 0) {
        s += sprintf(s, "&windgustdir=%d", wxdp->windgust.direction);
    }
#endif
    if (WXD_ISVALID(wxdp->outdoortemp)) {
        s += sprintf(s, "&tempf=%.1f", WXD_GETDAT(wxdp->outdoortemp, floatd));
    }
    if (WXD_ISVALID(wxdp->rainrate)) {
        s += sprintf(s, "&rainin=%.2f", WXD_GETDAT(wxdp->rainrate, floatd));
    }
    if (WXD_ISVALID(wxdp->rainday)) {
        s += sprintf(s, "&dailyrainin=%.2f", WXD_GETDAT(wxdp->rainday, floatd));
    }
    if (WXD_ISVALID(wxdp->barometer)) {
        s += sprintf(s, "&baromin=%.3f", WXD_GETDAT(wxdp->barometer, floatd));
    }
    if (WXD_ISVALID(wxdp->outdoorhum)) {
        s += sprintf(s, "&humidity=%.0f", WXD_GETDAT(wxdp->outdoorhum, floatd));
    }
    if (WXD_ISVALID(wxdp->outdoordewpoint)) {
        s += sprintf(s, "&dewptf=%.1f", WXD_GETDAT(wxdp->outdoordewpoint, floatd));
    }
    if (WXD_ISVALID(wxdp->solar)) {
        s += sprintf(s, "&solarradiation=%d", WXD_GETDAT(wxdp->solar, intd));
    }
    s += sprintf(s, "&softwaretype=fwx%%20v%d.%d&action=updateraw'", VERSION_MAJ, VERSION_MIN);
/*    sprintf(s, "&action=updateraw'"); */
#ifdef DEBUG_AERIS
    printf("\"%s\"\n", str);
#endif /*DEBUG_AERIS*/
    (void)system(str);
}

static int
wxident(int fd)
{
    int rc;
    int i;
    char cmd[8];
    unsigned char ident;

    sprintf(cmd, "WRD%c%c%c", 0x12, 0x4d, '\r');

    i = 0;
    do {
        if (++i > 4) {
            fprintf(stderr, "wxident - failed to wakeup station\n");
            return -1;
        }
    } while (wxwakeup(fd) != 0);

    if (wxcmd(fd, cmd) == -1) {
        fprintf(stderr, "wxident - wxcmd WRD 0x%02x 0x%02x 0x%02x failed\n",
                cmd[3]&0xff, cmd[4]&0xff, cmd[5]&0xff);
                return -1;
    }

    if ((rc = wxread(fd, &ident, 1, 5)) != 1) {
        fprintf(stderr, "wxident - wxread failed, rc = %d\n", rc);
        return -1;
    }
    return ident & 0xff;
}
