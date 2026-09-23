# dlr - Deliver for Minimal-OS

Client **and** server for [Deliver](https://github.com/fsminecrafter/Deliver), the LAN package manager. One program, `dlr.run`.

Run `dhcp` in the terminal once per boot first.

## Getting packages

```
dlr scan                       find servers on the LAN
dlr add <ip>[:port]            register a server by address
dlr servers-tui                browse servers and download packages
dlr list [server]              a server's packages
dlr search <query> [server]
dlr ping [server]
dlr install <pkg> [server]     download, verify SHA-256, install
```

`--password <pw>` for servers that require one. A package installs according to the `[Install.minimalos]` section of its `.pkg` manifest (`copy=`, `target=`).

`servers-tui` shows known servers, uses `R` to refresh discovery, and accepts a
server number followed by Enter. Choose a package number on the server screen
to download its verified `.mpkg` or `.tar` archive into the terminal's current
directory. Deliver servers provide packages, not arbitrary individual files.

## Serving packages

```
dlr present <dir> [name]       copy a package directory into the store and archive it
dlr packages                   what is being offered
dlr rebuild <name>             re-archive after editing files in the store
dlr unpresent <name>
dlr serve [--name n] [--port p] [--password pw]     (Ctrl+C to stop)
```

The store is `0:/var/dlrd/packages/`: one directory per package plus `<name>.mpkg` beside it. `present` builds the `.mpkg` once, so a request is just a file read, and two clients asking for the same package cannot race on the archiver.

`serve` broadcasts a hello every 5 s (which is what `dlr scan` on other machines hears) and accepts connections. Each client is served by its **own process** (this program re-run as `dlr --conn <handle> <ip> ...`); the kernel hands the connection over and closes it if that process dies. At most 3 clients are served at once, and a client that is silent for 5 s during the handshake is dropped.

## Archive format

A Minimal-OS server sends its native **`.mpkg`**; Linux and Windows servers send a tar. This client reads both. The first `INSTALL_DATA` frame says which:

```
SIZE:<bytes>                              tar   (unchanged)
SIZE:<bytes>|FORMAT=mpkg|OS=minimalos     .mpkg
```

Clients that predate the field still read the size (they stop at the first non-digit) but cannot extract an `.mpkg`; the current C++ client says so and refuses to install (`dlr download` keeps the file). Tar output from a Minimal-OS server may come later.

## Wire protocol differences from the C++ server

- A client that skips the `KEY:` exchange is dropped instead of being served in plaintext.
- A failed password attempt costs the client a one-second delay.

## Layout

| file | role |
|---|---|
| `main.c` | commands, install pass |
| `dlr_proto.c` | framing, session, requests |
| `dlr_crypto.c` | SHA-256, AES-256-GCM, base64 (no heap use per frame) |
| `dlr_tar.c`, `dlr_mpkg.c` | in-memory extractors feeding the same install pass |
| `dlr_server.c` | accept loop, hello broadcast, per-connection session |
| `dlr_registry.c` | the package store |
| `dlr_port_mos.c` | everything that touches the OS (a host build swaps this file) |

## Tests

`tests/dlr-host/` builds the same code against a Linux backend:

```
tests/dlr-host/build.sh                      build dlr-host
MOS_KERNEL=../Minimal-OS tests/dlr-host/mpkg_test.sh    .mpkg reader vs the kernel's compressor, under ASan/UBSan
MOS_KERNEL=../Minimal-OS DELIVER_BUILD=../Deliver/build tests/dlr-host/e2e.sh
```

None of this runs on the real kernel; see `tests/net-host` in the Minimal-OS repo for the TCP layer.
