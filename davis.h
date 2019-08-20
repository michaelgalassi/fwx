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
 * All questions & comments should be directed to me, michael at
 * galassi dot us.
 */

/*
 * What procs needs this?  PowerPC?  Alpha?
 */
#if defined(SWAP_DAVIS_DATA)
#define get_d_8(x) (x)
#define get_d_16(x) (((x)<<8&0xff00) | ((x)>>8&0x00ff))
#else
#define get_d_8(x) (x)
#define get_d_16(x) (x)
#endif

/*
 * For whatever reason the people who designed and implemented this
 * interface didn't feel the need to consider alignment or byte
 * ordering issues.  They also ignored the natural tendency software
 * people have to align things on power of two boundaries.
 *
 * The uint16_t elements will want to be byte reversed for use on
 * some boxen, the ia32 & amd64 are fine though.
 */
#pragma pack(1)
typedef struct vploopdata {
    char sig[3];                        /* "LOO" */
    int8_t barTrend;                    /* -60, -20, 0, 20, or 60 */
    char type;                          /* 0x00 */
    uint16_t nextRecord;                /* index to next record */
    uint16_t bar;                       /* barometric pressure */
    int16_t tempIn;                     /* indoor temperature */
    uint8_t humIn;                      /* indoor humidity */
    int16_t tempOut;                    /* outdoor temperature */
    uint8_t windSpeed;                  /* instantaneous wind speed */
    uint8_t windSpeed10;                /* 10 minute average wind speed */
    uint16_t windDir;                   /* wind direction */
    uint8_t tempOutExt[15];             /* temp from hum/temp stations */
    uint8_t humOut;                     /* outdoor humidity */
    uint8_t humOutExt[7];               /* hum from hum/temp stations */
    uint16_t rainRate;                  /* rain rate in/hour * 100 */
    uint8_t uv;                         /* UV intensity */
    uint16_t solarRad;                  /* solar radiation level */
    uint16_t rainStorm;                 /* inches in current storm */
    uint16_t rainStormDate;             /* start of current storm */
    uint16_t rainDay;                   /* inches * 100 today */
    uint16_t rainMonth;                 /* inches * 100 this month */
    uint16_t rainYear;                  /* inches * 100 this year */
    uint16_t etDay;                     /* evapo-transpiration for the day */
    uint16_t etMonth;                   /* evapo-transpiration for the month */
    uint16_t etYear;                    /* evapo-transpiration for the year */
    uint8_t soilMoist[4];               /* soil moisture */
    uint8_t leafWet[4];                 /* leaf wetness */
    uint8_t inAlamrs;                   /* inside alarms */
    uint8_t rainAlarms;                 /* rain alarms */
    uint16_t outAlarms;                 /* outside alarms */
    uint8_t tempHumAlarms[8];           /* temp & humidity moisture */
    uint8_t leafSoilAlarms[4];          /* leaf & soil alarms */
    uint8_t txBatStatus;                /* Tx battery status */
    uint16_t batCounts;                 /* battery counts */
    uint8_t forecastIcons;              /* forcast icon index */
    uint8_t forecastRule;               /* forecast rule number */
    uint16_t sunrise;                   /* sunrise (100*h+m) */
    uint16_t sunset;                    /* sunset (100*h+m) */
    uint8_t nl;                         /* \n */
    uint8_t ret;                        /* \r */
    uint16_t crc;                       /* two bytes of crc */
} vploopdata_t;
#pragma pack()

#define VPLOOPSIZE sizeof(vploopdata_t)

#define VPLOOPCMD "LOOP 01\n"
#define IDENT_VP 0x10

/*
 * Offsets on non-vantage-pro models (wizard-III, monitor-II, etc...
 * Not currently supported but why throw the info away, someday I
 * would like to add that support back.
 */
#pragma pack(1)
typedef struct dloopdata {
    char sig;                           /* 0x00 */
    int16_t tempIn;                     /* indoor temperature */
    int16_t tempOut;                    /* outdoor temperature */
    uint8_t windSpeed;                  /* instantaneous wind speed */
    uint16_t windDir;                   /* wind direction */
    uint16_t bar;                       /* barometric pressure */
    uint8_t humIn;                      /* indoor humidity */
    uint8_t humOut;                     /* indoor humidity */
    uint16_t rainDay;                   /* inches * 100 today */
    uint8_t nl;                         /* \n */
    uint8_t ret;                        /* \r */
    uint16_t crc;                       /* two bytes of crc */
} loopdata_t;
#pragma pack()

#define DLOOPSIZE sizeof(dloopdata_t)
