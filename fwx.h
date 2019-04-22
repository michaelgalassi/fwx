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

/*
 * an attempt to hide the (ugly) nature of the data
 */
typedef struct wxd {
    union {
        float floatd;
        int intd;
        long longd;
        char chard;
        time_t timetd;
        void *voidp;
    } dat;
    unsigned int flags;
    char *units;
} wxd_t;

typedef struct wind {
    int speed;
    int direction;
} wind_t;

typedef struct wxdat {
    time_t time;                /* time this sample was taken at */
    wind_t windcur;             /* current wind speed/direction */
    wind_t windavg;             /* average wind speed/direction */
    wind_t windgust;            /* 10 minute wind gust speed/direction */
    wxd_t barometer;            /* barometric pressure */
    wxd_t windspeed;            /* current windspeed */
    wxd_t winddir;              /* current windspeed's direction */
    wxd_t avgwindspeed;         /* average wind speed */
    wxd_t avgwindspeedinterval; /* how long the average is taken over (min) */
    wxd_t indoortemp;           /* indoors temperature */
    wxd_t outdoortemp;          /* outdoors temperature */
    wxd_t indoorhum;            /* indoors humidity */
    wxd_t outdoorhum;           /* outdoors humidity */
    wxd_t outdoordewpoint;      /* outdoors dewpoint */
    wxd_t rainrate;             /* current rain-rate */
#ifdef NOTYET
    wxd_t rainhour;             /* rain in past hour */
#endif /*NOTYET*/
    wxd_t rainday;              /* rain today */
    wxd_t rainmonth;            /* rain this month */
    wxd_t rainyear;             /* rain this year */
} wxdat_t;

#define WXD_GETFLAGS(x)         ((x).flags)
#define WXD_SETFLAGS(x, n)      ((x).flags = (n))
#define WXD_GETDAT(x, fmt)      ((x).dat.fmt)
#define WXD_SETDAT(x, v, fmt)   ((x).dat.fmt = (v))
#define WXD_GETUNITS(x)         ((x).units)
#define WXD_SETUNITS(x, s)      ((x).units = (s))

/* flags */
#define WXD_INVALID     0x0000          /* element has no data */
#define WXD_VALID       0x0010          /* element has valid data */
#define WXD_METRIC      0x0100          /* element is in metric units */
#define WXD_ENGLISH     0x0200          /* element is in english units */

#define WXD_ISVALID(x)          (WXD_GETFLAGS(x) & WXD_VALID)
#define WXD_ISMETRIC(x)         (WXD_GETFLAGS(x) & WXD_METRIC)
#define WXD_ISENGLISH(x)        (WXD_GETFLAGS(x) & WXD_ENGLISH)

/*
 * low 4 bits are the number of significant places after the decimal
 * point in the float.  eg. data that should really have been an int
 * such as wind direction would set this to 0, data that has been
 * scaled by 1000 would set this to 3.  Note that this is not an
 * indication of accuracy or even resolution, just of the weather
 * station's data storage format.
 */
#define WXD_VALDECMASK          0x000f          /* mask valid dec places */
#define WXD_GVPLACES(x)         (WXD_GETFLAGS(x) & 0x0f)
#define WXD_SVPLACES(x, n)      {(x)->flags &= ~0x0f;(x)->flags |= (n) & 0x0f;}

/*
 * returns from read loop routine
 */
#define WXOK    0               /* yipee */
#define WXTMOUT 1               /* need new loop command */
#define WXERR   2               /* some other error :-( */
