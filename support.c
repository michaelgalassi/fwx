/*
 * FreeWX - logger for Davis weather stations
 *
 * Copyright 2003-2019 Michael Galassi, all rights reserved.
 *
 * This code may be re-distributed under the terms and conditions of
 * the BSD 2 clause license.
 *
 * This code is loosely based on Alan Batie's wiz3d.
 *
 * All questions & comments should be directed to me at michael at
 * galassi dot us.
 */

#include <sys/types.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <termios.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>

#ifndef IF_SPEED
#define IF_SPEED 19200          /* default, override on cc command line */
#endif /*IF_SPEED*/

#define MAX_TIMEOUT 30          /* the longest time station can take to xmit */
#define MAX_READ 256            /* the longest data station can xmit */
#define ACK 0x06                /* station ACKs commands with this */

/* used for debugging */
#define LINELEN 80              /* length of a line */
#define CHARLEN 5               /* width to display one char */

static void __inline
dumpbyte(unsigned char c, char *buf)
{
#if 0
    if (isprint(c)) {
        sprintf(buf, "   %c ", c);
    } else {
#endif
        sprintf(buf, "0x%02x ", c);
#if 0
    }
#endif
}

void
dumpbuf(FILE *stream, unsigned char *p, size_t len)
{
    int i;
    char buf[LINELEN+2];        /* text, newline, & null terminator */

    i = 0;
    while (len-- > 0) {
        dumpbyte(*p++, &buf[i*CHARLEN]);
        if (++i >= LINELEN/CHARLEN || len == 0) {
            strcat(&buf[i*CHARLEN], "\n");
            fputs(&buf[0], stream);
            i = 0;
        }
    }
    return;
}

int
wxopen(char *devname)
{
    struct termios termios;
    char *wxdev;
    int wxfd;

    /* validate (and maybe complete) device name */
    if (*devname == '/') {
        wxdev = devname;
    } else {
        wxdev = alloca(strlen(devname)+sizeof("/dev/")+1);
        strcpy(wxdev, "/dev/");
        strcat(wxdev, devname);
    }

#ifdef DEBUG_WXOPEN
    fprintf(stderr, "wxopen - device - %s\n", wxdev);
#endif /*DEBUG_WXOPEN*/

    if ((wxfd = open(wxdev, O_RDWR|O_NONBLOCK)) == -1) {
        perror("wxopen - open");
        return -1;
    }

    /* set for our use only */
    if (ioctl(wxfd, TIOCEXCL, NULL) == -1) {
        perror("wxopen - ioctl(TIOCEXCL)");
        (void)close(wxfd);
        return -1;
    }

    /*
     * clear NONBLOCK flag as it should no longer be needed
     */
    if (fcntl(wxfd, F_SETFL, 0) == -1) {
        perror("wxopen - fcntl(F_SETFL)");
        (void)close(wxfd);
        return -1;
    }

    if (tcgetattr(wxfd, &termios) != 0) {
        perror("wxopen - tcgetattr");
        (void)close(wxfd);
        return -1;
    }

    cfmakeraw(&termios);                /* redundant */
    termios.c_iflag = IGNBRK;
    termios.c_oflag = 0;
    termios.c_cflag = CS8|CREAD|CLOCAL;
    termios.c_lflag = NOKERNINFO;
    termios.c_cc[VMIN] = 0;
    termios.c_cc[VTIME] = 0;
    cfsetspeed(&termios, IF_SPEED);

    if (tcsetattr(wxfd, TCSAFLUSH, &termios) == -1) {
        perror("wxopen - tcsetattr");
        (void)close(wxfd);
        return -1;
    }

#ifdef DEBUG_WXOPEN
    fprintf(stderr, "wxopen complete\n");
#endif /*DEBUG_WXOPEN*/
    return wxfd;
}

int
wxsettimeout(int fd, int timeout)
{
    struct termios termios;

    if (tcgetattr(fd, &termios) == -1) {
        perror("wxsettimeout - tcgetattr");
        return -1;
    }

    termios.c_cc[VMIN] = 0;
    termios.c_cc[VTIME] = timeout*10;

    if (tcsetattr(fd, TCSANOW|TCSASOFT, &termios) == -1) {
        perror("wxsettimeout - tcsetattr");
        return -1;
    }
    return 0;
}

int
wxread(int fd, void *buf, size_t len, int timeout)
{
    time_t expire;
    ssize_t remaining;
    ssize_t rc;
    char *bufp;

    if (timeout <= 0 || timeout > MAX_TIMEOUT) {
        fprintf(stderr, "wxread - timeout %d out of range\n", timeout);
        return -1;
    }

    if (len == 0 || len > MAX_READ) {
        fprintf(stderr, "wxread - length %zu out of range\n", len);
        return -1;
    }

    expire = time((time_t *)NULL) + timeout;

    remaining = len;
    bufp = (char *)buf;

    while (remaining > 0 && timeout > 0) {
        if (wxsettimeout(fd, timeout) == -1) {
            fprintf(stderr, "wxread - failed to set timeout\n");
            return -1;
        }
        if ((rc = read(fd, bufp, remaining)) != remaining) {
            if (rc == -1) {
                perror("wxread - read");
                return -1;
            }
#ifdef DEBUG_WXREAD
            fprintf(stderr, "wxread - read got %zd expected %zd\n",
                    rc, remaining);
#endif /*DEBUG_WXREAD*/
        }
        remaining -= rc;
        bufp += rc;
        timeout = expire - time((time_t *)NULL);
    }
#ifdef DEBUG_WXREAD
    dumpbuf(stderr, buf, len - remaining);
#endif /*DEBUG_WXREAD*/

    return (len - remaining);
}

int
wxflush(fd)
{
    struct termios termios;

    if (tcgetattr(fd, &termios) == -1) {
        perror("wxflush - tcgetattr");
        return -1;
    }

    if (tcsetattr(fd, TCSAFLUSH|TCSASOFT, &termios) == -1) {
        perror("wxflush - tcsetattr");
        return -1;
    }
    return 0;
}

int
wxwakeup(fd)
{
    ssize_t rc;
    char resp[2];

    if (wxflush(fd) != 0) {
        fprintf(stderr, "wxwakeup - flush failed\n");
        return -1;
    }
    if ((rc = write(fd, &"\n", 1)) != 1) {
        if (rc == -1) {
            perror("wxwakeup - write");
            return -1;
        }
        fprintf(stderr, "wxwakeup - newline send failed\n");
        return -1;
    }

    if (wxread(fd, (void *)&resp, sizeof(resp), 5) == -1) {
        fprintf(stderr, "wxwakeup - wxread failed\n");
        return -1;
    }
    /* Davis doesn't make it clear which to expect */
    if ((resp[0] != '\r' && resp[1] != '\n') &&
        (resp[0] != '\n' && resp[1] != '\r')) {
#ifdef DEBUG_WXWAKEUP
        fprintf(stderr, "wxwakeup - didn't get expected bytes\n"
                "wxwakeup - got 0x%x 0x%x\n",
                resp[0] & 0xff, resp[1] & 0xff);
#endif /*DEBUG_WXWAKEUP*/
        return -1;
    }

    return 0;
}

int
wxgetack(int fd)
{
    int i;
    unsigned char ackbuf;

    i = 0;
    do {
        if (wxread(fd, (void *)&ackbuf, sizeof(ackbuf), 1) == -1) {
            fprintf(stderr, "wxgetack - wxread failed\n");
            return -1;
        }
        if (i++ >= 5) {
            fprintf(stderr, "wxgetack - failing after %d attempts\n", i);
            return -1;
        }
    } while (ackbuf != ACK);
    return 0;
}

int
wxcmd(int fd, char *cmd)
{
    size_t len;
    ssize_t rc;

    len = strlen(cmd);
    if ((rc = write(fd, cmd, len)) == -1) {
        perror("wxcmd - write");
        return -1;
    }
    if (rc != (ssize_t)len) {
        fprintf(stderr, "wxcmd - write expected %zu, got %zd\n", len, rc);
        return -1;
    }

    if (wxgetack(fd) != 0) {
        fprintf(stderr, "wxcmd - failed getting cmd ack\n");
        return -1;
    }
#ifdef DEBUG_WXCMD
    fprintf(stderr, "wxcmd - sent command %s\n", cmd);
#endif /*DEBUG_WXCMD*/
    return 0;
}
