/*
 * nettest - exercises every piece of SYS_NET, in the order the
 * Deliver client will use them.
 *
 * Build:   ./build.sh examples/nettest.c
 * Import:  nettest.run into MinimaFS
 * Run:     dhcp                       (in the terminal, first - a .run
 *                                      program is not allowed to do this)
 *          run 0:/programs/nettest.run
 *          run 0:/programs/nettest.run example.com
 *          run 0:/programs/nettest.run --discover
 *
 * Failures here are stack problems, not client problems - which is the
 * whole point of having it before writing the client.
 */

#include "minimalos.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "net.h"

#define DISCOVERY_PORT 4243

static void print_status(void) {
    syscall_net_status_t st;
    if (mos_net_status(&st) != (long)SYS_SUCCESS) {
        printf("net: SYS_NET_STATUS failed\n");
        return;
    }

    if (!st.has_driver) {
        printf("net: no NIC driver registered\n");
        return;
    }

    printf("net: MAC %x:%x:%x:%x:%x:%x\n",
           st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);

    if (!st.configured) {
        printf("net: no IP configured - run 'dhcp' in the terminal first\n");
        return;
    }

    char ip[16], mask[16], gw[16], dns[16];
    mos_ip_to_string(st.local_ip, ip);
    mos_ip_to_string(st.netmask, mask);
    mos_ip_to_string(st.gateway, gw);
    mos_ip_to_string(st.dns, dns);
    printf("net: addr %s  mask %s\n", ip, mask);
    printf("net: gw   %s  dns  %s\n", gw, dns);
}

static int test_heap_and_random(void) {
    // The client allocates its receive buffer at runtime, so prove the
    // heap works before trusting it with a download.
    unsigned char* buf = (unsigned char*)malloc(4096);
    if (!buf) { printf("heap: malloc(4096) failed\n"); return 0; }

    memset(buf, 0xAB, 4096);
    if (buf[0] != 0xAB || buf[4095] != 0xAB) {
        printf("heap: readback mismatch\n");
        free(buf);
        return 0;
    }

    unsigned char* grown = (unsigned char*)realloc(buf, 16384);
    if (!grown) { printf("heap: realloc failed\n"); free(buf); return 0; }
    if (grown[0] != 0xAB) { printf("heap: realloc lost contents\n"); free(grown); return 0; }
    free(grown);
    printf("heap: ok (malloc/realloc/free)\n");

    unsigned char key[32];
    memset(key, 0, sizeof(key));
    if (mos_random_bytes(key, sizeof(key)) != (long)sizeof(key)) {
        printf("random: SYS_RANDOM failed\n");
        return 0;
    }

    // Not a randomness test - just catches the stub-returns-zeroes
    // failure mode, which would silently produce a fixed session key.
    int nonzero = 0;
    for (int i = 0; i < 32; i++) if (key[i]) nonzero++;
    printf("random: 32 bytes, %d non-zero, first %x %x %x %x\n",
           nonzero, key[0], key[1], key[2], key[3]);
    return nonzero > 0;
}

static int test_http_get(const char* host) {
    printf("resolving %s ...\n", host);
    uint32_t ip = mos_net_lookup(host, 5000);
    if (!ip) { printf("  resolve failed\n"); return 0; }

    char ipstr[16];
    mos_ip_to_string(ip, ipstr);
    printf("  -> %s\n", ipstr);

    long conn = mos_tcp_connect(ip, 80, 5000);
    if (conn <= 0) { printf("  connect failed (%d)\n", (int)conn); return 0; }
    printf("  connected, handle %d\n", (int)conn);

    char request[256];
    int len = 0;
    const char* parts[] = { "HEAD / HTTP/1.0\r\nHost: ", host,
                            "\r\nConnection: close\r\nUser-Agent: MinimalOS-nettest\r\n\r\n" };
    for (int i = 0; i < 3; i++) {
        const char* p = parts[i];
        while (*p && len < (int)sizeof(request) - 1) request[len++] = *p++;
    }
    request[len] = '\0';

    if (!mos_tcp_send_all(conn, request, (uint32_t)len)) {
        printf("  send failed\n");
        mos_tcp_close(conn);
        return 0;
    }

    char chunk[513];
    long total = 0;
    long idle = mos_uptime();
    int printed_status = 0;

    for (;;) {
        long n = mos_tcp_recv(conn, chunk, sizeof(chunk) - 1);
        if (n < 0) break;                       // peer closed, drained
        if (n == 0) {
            if (mos_uptime() - idle > 8000) break;
            mos_sleep(5);
            continue;
        }
        idle = mos_uptime();
        total += n;
        if (!printed_status) {
            chunk[n] = '\0';
            char* eol = strchr(chunk, '\r');
            if (eol) *eol = '\0';
            printf("  %s\n", chunk);
            printed_status = 1;
        }
    }

    mos_tcp_close(conn);
    printf("  received %d bytes total\n", (int)total);
    return total > 0;
}

static int test_discovery(void) {
    // Exactly what the Deliver client does: bind 4243, broadcast
    // DLR_DISCOVER, collect "DLR|name|needs_pw|port" replies, and also
    // pick up the unsolicited hello every server broadcasts on a timer.
    long sock = mos_udp_bind(DISCOVERY_PORT);
    if (sock <= 0) { printf("udp: bind %d failed (%d)\n", DISCOVERY_PORT, (int)sock); return 0; }
    printf("udp: bound %d, broadcasting DLR_DISCOVER\n", DISCOVERY_PORT);

    const char* probe = "DLR_DISCOVER\n";
    long sent = mos_udp_send(sock, SYSCALL_NET_IP_BROADCAST, DISCOVERY_PORT,
                             DISCOVERY_PORT, probe, (uint32_t)strlen(probe));
    if (sent < 0) printf("udp: broadcast send failed (%d)\n", (int)sent);

    char buf[513];
    uint32_t from_ip = 0;
    uint16_t from_port = 0;
    int found = 0;
    long deadline = mos_uptime() + 4000;

    while (mos_uptime() < deadline) {
        long n = mos_udp_recv_timeout(sock, buf, sizeof(buf) - 1,
                                      &from_ip, &from_port, 500);
        if (n <= 0) continue;
        buf[n] = '\0';

        char ipstr[16];
        mos_ip_to_string(from_ip, ipstr);
        if (strncmp(buf, "DLR|", 4) == 0) {
            printf("  server at %s: %s", ipstr, buf);
            found++;
        } else {
            printf("  (%d bytes from %s, not a DLR hello)\n", (int)n, ipstr);
        }
    }

    mos_udp_unbind(sock);
    if (!found) {
        printf("udp: no servers heard.\n");
        printf("     Under QEMU '-netdev user' the guest is NATed and LAN\n");
        printf("     broadcasts never reach it. Use a tap/bridge netdev to\n");
        printf("     test discovery for real.\n");
    }
    return found;
}

int main(int argc, char** argv) {
    printf("=== Minimal-OS nettest ===\n");
    print_status();

    if (!mos_net_is_up()) {
        printf("Interface is not up; stopping here.\n");
        return 1;
    }

    test_heap_and_random();

    int discover = 0;
    const char* host = "example.com";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--discover") == 0) discover = 1;
        else host = argv[i];
    }

    if (discover) {
        test_discovery();
    } else {
        test_http_get(host);
        printf("(pass --discover to test UDP LAN discovery instead)\n");
    }

    printf("=== done ===\n");
    return 0;
}
