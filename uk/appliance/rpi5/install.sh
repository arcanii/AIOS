#!/bin/sh
# install.sh -- deploy the "boots into AIOS" LIGHT PATH on a systemd Linux host (e.g. the RPi5).
#
# Stages the AIOS kernel + userland under /opt/aios and installs the aios-console.service systemd unit,
# so the machine presents the AIOS system layer (init -> login -> a user session) on a console (tty8 by
# default) at boot. It does NOT touch the host boot chain, the primary getty (tty1), or ssh, and it is
# fully REVERSIBLE. Idempotent. Run from the uk/ tree (needs a built aios-uk + sbase + dash: `make all`).
#
#   sudo sh appliance/rpi5/install.sh          # stage + install the unit (does NOT enable or start it)
#   sudo systemctl start  aios-console         # try it now (transient; switch to the tty: `sudo chvt 8`)
#   sudo systemctl enable --now aios-console   # boot into AIOS from now on (PERSISTENT)
#   # revert completely:
#   sudo systemctl disable --now aios-console
#   sudo rm -rf /opt/aios /etc/systemd/system/aios-console.service && sudo systemctl daemon-reload
#
# Knob: AIOS_TTY=ttyN chooses the console (default tty8 -- outside systemd's autovt range 1..6, so it
# does not fight logind's on-demand gettys). For a SERIAL console use e.g. AIOS_TTY=ttyAMA0 and set
# TERM=vt220 in the unit.
set -eu

UK=$(cd "$(dirname "$0")/../.." && pwd)      # the uk/ tree (this script lives in uk/appliance/rpi5/)
SELF=$(cd "$(dirname "$0")" && pwd)
DEST=/opt/aios
TTY=${AIOS_TTY:-tty8}

[ "$(id -u)" = 0 ] || { echo "run as root: sudo sh $0" >&2; exit 1; }
test -x "$UK/aios-uk" || { echo "build first: (cd $UK && make all)  -- missing aios-uk" >&2; exit 1; }
test -x "$UK/dash"    || { echo "build first: (cd $UK && make all)  -- missing dash" >&2; exit 1; }

echo "[install] staging the AIOS root + kernel under $DEST ..."
mkdir -p "$DEST"
sh "$UK/mkaiosroot.sh" "$DEST/aiosroot" >/dev/null
cp "$UK/aios-uk" "$DEST/aios-uk"
cp "$SELF/README.rpi5.md" "$DEST/README.rpi5.md" 2>/dev/null || true

echo "[install] installing the systemd unit on /dev/$TTY ..."
sed "s|/dev/tty8|/dev/$TTY|g; s|UtmpIdentifier=tty8|UtmpIdentifier=$TTY|g" \
    "$SELF/aios-console.service" > /etc/systemd/system/aios-console.service
systemctl daemon-reload

echo "[install] done."
echo "  AIOS is staged at $DEST; the aios-console unit is installed but NOT enabled/started."
echo "  try now:      sudo systemctl start aios-console   (then: sudo chvt ${TTY#tty}  to view it)"
echo "  boot into it: sudo systemctl enable --now aios-console"
echo "  status:       systemctl status aios-console ; ps --ppid \$(systemctl show -p MainPID --value aios-console)"
echo "  revert:       sudo systemctl disable --now aios-console; sudo rm -rf $DEST /etc/systemd/system/aios-console.service; sudo systemctl daemon-reload"
