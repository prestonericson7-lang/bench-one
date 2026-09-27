#!/bin/bash
# accel-firstboot.sh -- run by accel-firstboot.service at boot until the staged accelerator bundle has
# installed once (stage_pi_card.sh puts the bundle at /opt/accel/bundle and enables the unit).
# Log: /var/lib/accel/firstboot.log, copied to the desktop user's home as accel-install.log.
B=${ACCEL_BUNDLE:-/opt/accel/bundle}
S=${ACCEL_STATE:-/var/lib/accel}
mkdir -p "$S"
u=$(getent passwd 1000 | cut -d: -f1,6)
# the desktop user, as "sudo bash install_pi.sh" from that account would pass it (pi_setup.sh: dialout)
[ -n "$u" ] && export SUDO_USER=${u%%:*}
{ echo "=== accel first boot, $(date)"; bash "$B/install_pi.sh"; } >> "$S/firstboot.log" 2>&1
rc=$?
echo "=== install_pi.sh exit $rc" >> "$S/firstboot.log"
if [ -n "$u" ] && [ -d "${u#*:}" ]; then
  cp -f "$S/firstboot.log" "${u#*:}/accel-install.log" && chown "${u%%:*}": "${u#*:}/accel-install.log"
fi
# done only when it worked; otherwise the next boot tries again (install_pi.sh is idempotent)
[ $rc = 0 ] && date > "$S/firstboot.done"
exit 0
