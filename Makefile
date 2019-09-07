# Makefile for fwx
#
# You might need to add "-DSWAP_DAVIS_DATA" to CFLAGS, look at davis.h
# for more info. (hint, ppc needs this, ia32/amd64 do NOT)

BINARY=fwx
BASEDIR=/usr/local
INSTALL=install -c -m 0755
CONFIG=fwx.conf.sample
RC=fwx.sh
SRCS=fwx.c support.c crc.c fwx.h davis.h
OBJS=fwx.o support.o crc.o
CFLAGS=-g -O2 -std=c11 -Wall -Wextra -Werror -pedantic -DIF_SPEED=19200 -DVP -DDEBUG_CWOP
LDFLAGS=-lm

${BINARY}: ${OBJS}
	${CC} ${LDFLAGS} ${OBJS} -o ${BINARY}

fwx.o: fwx.c davis.h fwx.h
support.o: support.c davis.h
crc.o: crc.c

install: ${BINARY} ${CONFIG} ${RC}
	${INSTALL} ${BINARY} ${BASEDIR}/bin
	${INSTALL} ${RC} ${BASEDIR}/etc/rc.d/fwx
	${INSTALL} ${CONFIG} ${BASEDIR}/etc

clean:
	rm -f ${BINARY} ${OBJS}

dist:
	tar czf fwx.tar.gz Makefile ${SRCS} ${CONFIG} ${RC}
