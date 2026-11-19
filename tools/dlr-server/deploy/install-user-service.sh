#!/bin/sh
# Installs dlrd as a systemd *user* service (no root) with its own copy of the
# program, so it keeps running if the source checkout changes. Re-run to update.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST=${DLRD_HOME:-$HOME/.local/share/dlrd}
mkdir -p "$DEST" "$HOME/.config/systemd/user"
cp "$HERE/../dlrd.py" "$DEST/dlrd.py"
# Carry an existing data directory over once.
if [ ! -e "$DEST/data" ] && [ -d "$HERE/../data" ]; then cp -a "$HERE/../data" "$DEST/data"; fi
cat > "$HOME/.config/systemd/user/dlrd.service" <<UNIT
[Unit]
Description=dlrd - Deliver server for Minimal-OS
After=network-online.target

[Service]
ExecStart=/usr/bin/python3 $DEST/dlrd.py --data $DEST/data
Restart=on-failure
RestartSec=3
NoNewPrivileges=yes
PrivateTmp=yes

[Install]
WantedBy=default.target
UNIT
cat > "$HOME/.config/systemd/user/duckdns.service" <<UNIT
[Unit]
Description=Update DuckDNS

[Service]
Type=oneshot
ExecStart=/bin/sh $HERE/duckdns-update.sh %i
UNIT
systemctl --user daemon-reload
systemctl --user enable dlrd.service
systemctl --user restart dlrd.service
loginctl enable-linger "$USER" 2>/dev/null || echo "note: run 'sudo loginctl enable-linger $USER' so it starts at boot without a login"
systemctl --user --no-pager status dlrd.service | head -5
