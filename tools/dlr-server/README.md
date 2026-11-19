# dlrd - Deliver server for Linux

A Python Deliver server for Minimal-OS. It speaks the native dlr protocol, so the
`dlr` client on Minimal-OS works against it unchanged, and it also runs a website
and an HTTP API.

```
pip install -r requirements.txt
python3 dlrd.py --open          # data goes in ./data, admin window opens in your browser
```

## What it serves

| What | Where | Notes |
|---|---|---|
| dlr protocol | TCP 7870 | `dlr add xlabs.duckdns.org:7870`, `dlr list`, `dlr install <pkg>` |
| Home page | `http://host/` | name, package count, how to install |
| Package list | `http://host/list` | every package with its files, sizes, checksum |
| API list | `GET /api/package` | JSON; `?q=text` searches |
| API download | `GET /api/package/<name>` | the tar archive; `X-DLR-SHA256` has its checksum |
| API details | `GET /api/package/<name>/info` | JSON including the file list |
| API endpoint info | `GET /api/endpoint` | JSON: where the dlr, UDP and web sides live |
| dlr over UDP | UDP 7870 | `dlr add udp://host:port`, for UDP-only tunnels such as playit.gg |
| Admin window | `http://127.0.0.1:8081` | drag a folder in, Settings button, file listing |

With a password set, `/list` and `/api/*` need it (header `X-DLR-Password: pw`, or
Basic auth with any user name). The home page stays public.

The archive is a tar with every entry under `<name>/`, which is exactly what the
C++ Deliver server sends and what `dlr` extracts. Downloads are checked against
SHA-256 by the client.

## Over UDP (playit.gg and friends)

Some connections only get UDP through a tunnel. dlrd answers the dlr operations
(list, search, ping, download) over UDP on `udp_port` (default 7870; UDP and TCP
port numbers do not clash). Point a playit.gg **UDP** tunnel at `127.0.0.1:7870`,
then in Settings put the address playit gives you into *Public UDP address*.

- `dlr add udp://<playit-host>:<port>` on Minimal-OS adds it directly.
- `dlr add http://<your-website>[:port]` asks the website (`/api/endpoint`) where the
  server is and is **redirected to the UDP address**. So does a bare `dlr add <host>`
  when nothing answers on the dlr port but the website on port 80 does.
- The protocol ("DLU1") is documented in `dlrd.py`. It is stateless and the client
  re-asks for lost chunks (tested at 20% packet loss). A cookie bound to the sender's
  address stops spoofed requests from turning the server into a traffic amplifier.
  A password is never sent; a proof derived from it is. Like the TCP protocol, the
  data itself is not encrypted.

## Admin window

- **Drag a folder** onto the window to publish it as a package named after the
  folder. Loose files become one package; type its name first. A `.pkg` manifest
  is generated if the folder has none. Edit `[Install.minimalos]` in your own
  manifest to control where a package installs.
- **Settings**: server name, password, ports, workers, TLS files, admin password.
  Name and password apply immediately; ports, workers and TLS need *Save & restart*.
- You can also drop a folder straight into `data/packages/`; it is published within
  a few seconds.

The window only answers on `127.0.0.1` unless you set an admin password. To reach
it from elsewhere, use an SSH tunnel (`ssh -L 8081:127.0.0.1:8081 server`).

## Multiple cores

The master binds the sockets and forks one worker process per core (`--workers N`
or the Settings field to change it); each worker accepts on the same sockets and
serves clients in threads. Workers read everything from disk, so there is nothing
to keep in sync.

## Putting it on xlabs.duckdns.org

1. Forward TCP **7870** (dlr), **7871** (website/API) and **7872** (HTTPS) from your router to this machine.
   If you are behind CGNAT this cannot work; use a VPS instead.
2. The default ports are all above 1024, so no special privilege is needed. The
   site is then `http://xlabs.duckdns.org:7871/` (port 80 would need
   `setcap cap_net_bind_service` or a router rule 80 -> 7871).
3. For HTTPS, get a certificate for the name (certbot or acme.sh, DNS-01 with the
   DuckDNS plugin), then in Settings keep the HTTPS port 7872 and set the two files
   (`fullchain.pem`, `privkey.pem`). Renewed certificates are picked up
   without a restart.
4. Keep DuckDNS pointing at your IP, e.g. a cron job calling
   `https://www.duckdns.org/update?domains=xlabs&token=...&ip=`.

## Good to know

- The **native dlr protocol is not secure by itself**: the session key travels in
  the clear and the password is sent unencrypted (see `programs/dlr/dlr_proto.h`).
  This server matches it so existing clients work. For anything sensitive, use
  the HTTPS API, and set a password.
- "Also answer dlr clients on the web ports" lets a dlr client connect to a web port
  when only those are open. It costs web clients nothing but adds a short
  wait before a silent connection is treated as dlr, so it is off by default.
- Wrong passwords are limited to 10 a minute per address and worker.
- Chunks are 4096 bytes because the Minimal-OS client rejects frames over 8 KiB.

## Tests

```
python3 test_dlrd.py
```

Builds the real Minimal-OS client (`tests/dlr-host`) and runs it against a live
server: list, search, ping, install (files compared byte for byte), eight parallel
installs, passwords, path-traversal names, the HTTP API, and admin uploads.
