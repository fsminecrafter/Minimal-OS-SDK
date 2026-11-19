#!/bin/sh
# Host firewall for a dlrd server, using ufw.   sudo sh firewall.sh [--dry-run]
#
# Policy: drop everything inbound except
#   - loopback, the Tailscale interface and libvirt's bridge (trusted, local)
#   - the three dlrd ports from anywhere:  7870 dlr, 7871 web/API, 7872 HTTPS
#   - Tailscale's own UDP 41641 (lets tailnet peers connect directly)
# SSH, RDP, VNC and the admin window are therefore NOT reachable from the
# local network or the internet; use them over Tailscale. Outbound is open.
#
# Safe to run twice. Nothing is reset, so existing rules you added stay.
set -eu
DRY=0; [ "${1:-}" = "--dry-run" ] && DRY=1
run() { if [ "$DRY" = 1 ]; then echo "+ $*"; else "$@"; fi; }
[ "$DRY" = 1 ] || [ "$(id -u)" = 0 ] || { echo "run with sudo (or --dry-run to preview)" >&2; exit 1; }
command -v ufw >/dev/null || { echo "ufw is not installed: sudo apt install ufw" >&2; exit 1; }

# Allow before enabling, so turning it on cannot cut off a Tailscale SSH session.
run ufw allow in on lo
run ufw allow in on tailscale0 comment 'tailnet'
run ufw allow in on virbr0 comment 'libvirt VMs'
run ufw allow 7870:7872/tcp comment 'dlrd'
run ufw allow 41641/udp comment 'tailscale direct'
# libvirt VM traffic is forwarded through the host.
run ufw route allow in on virbr0
run ufw route allow out on virbr0

run ufw default deny incoming
run ufw default allow outgoing
run ufw default deny routed
run ufw logging low
run ufw --force enable
run ufw status verbose
