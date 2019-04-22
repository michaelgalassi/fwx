#!/bin/sh

# PROVIDE: fwx
# REQUIRE: DAEMON
# BEFORE:  LOGIN
# KEYWORD: shutdown

# Add the following lines to /etc/rc.conf to enable fwx:
# fwx_enable="YES"
# fwx_flags="<set as needed>"

. /etc/rc.subr

name=fwx
rcvar=fwx_enable

command="/usr/local/bin/fwx"

fwx_enable=${fwx_enable:-"NO"}
fwx_flags=${fwx_flags:-"-b -l /var/fwx -d /dev/ttyU0 -i 5"}

load_rc_config $name
run_rc_command "$1"
