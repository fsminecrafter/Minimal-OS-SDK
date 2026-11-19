#!/bin/sh
# Keeps a DuckDNS name pointed at this machine's current public IP.
# Token: put it alone in ~/.config/duckdns/token (chmod 600), then
#     sh duckdns-update.sh xlabs
set -eu
DOMAIN=${1:?usage: duckdns-update.sh <subdomain, e.g. xlabs>}
TOKEN_FILE=${DUCKDNS_TOKEN_FILE:-$HOME/.config/duckdns/token}
[ -r "$TOKEN_FILE" ] || { echo "no token in $TOKEN_FILE" >&2; exit 1; }
# The token goes in the URL because that is how DuckDNS's API works; keep it
# out of logs by never echoing the request.
# An empty ip= makes DuckDNS use the address the request came from.
RESULT=$(curl -fsS --max-time 20 --get "https://www.duckdns.org/update" \
    --data-urlencode "domains=$DOMAIN" --data-urlencode "token=$(cat "$TOKEN_FILE")" --data "ip=")
echo "duckdns $DOMAIN: $RESULT"
[ "$RESULT" = "OK" ]
