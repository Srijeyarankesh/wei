/*
 * wei_sockdump.c -- minimal AF_UNIX SOCK_DGRAM sniffer for the OneWifi->wei
 * link-quality IPC (/tmp/linkquality_stats.sock).
 *
 * Binds the datagram socket path, receives each frame OneWifi sends, decodes the
 * 6-byte versioned TLV header, and prints the per-station MAC (mac_str is the
 * first field of stats_arg_t, at value byte 0) plus a short hexdump of each entry.
 * Self-contained: no wei/OneWifi headers required, so it cross-compiles with any
 * libc for the gateway.
 *
 * Only one process can own a DGRAM path, so run this INSTEAD of wei:
 *     systemctl stop wei
 *     ./wei_sockdump                 # Ctrl-C to stop (unlinks the socket)
 *     systemctl start wei
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PATH "/tmp/linkquality_stats.sock"
#define RCVBUF_BYTES (4 * 1024 * 1024)
#define TLV_HDR_LEN  6            /* type(1) ver(1) elem_size(2) len(2) */
#define HEX_PREVIEW  32           /* bytes of each entry to hexdump */

static volatile sig_atomic_t g_stop;
static char g_path[108];

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static const char *msg_type_str(unsigned t)
{
    switch (t) {
    case 1:  return "PERIODIC_STATS";
    case 2:  return "DISCONNECT";
    case 3:  return "RAPID_DISCONNECT";
    case 7:  return "REGISTER_STA";
    case 8:  return "UNREGISTER_STA";
    default: return "OTHER";
    }
}

static void ts_now(char *out, size_t n)
{
    struct tm tmv;
    time_t now = time(NULL);
    localtime_r(&now, &tmv);
    strftime(out, n, "%H:%M:%S", &tmv);
}

static void hex_preview(const uint8_t *p, size_t n)
{
    size_t i;
    if (n > HEX_PREVIEW) {
        n = HEX_PREVIEW;
    }
    printf("      hex:");
    for (i = 0; i < n; i++) {
        printf(" %02x", p[i]);
    }
    printf("%s\n", (n == HEX_PREVIEW) ? " ..." : "");
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : DEFAULT_PATH;
    struct sockaddr_un addr;
    uint8_t buf[65536];
    int rcvbuf = RCVBUF_BYTES;
    int fd;

    snprintf(g_path, sizeof(g_path), "%s", path);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket: %s\n", strerror(errno));
        return 1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    /* Take over the path: wei must be stopped first. */
    (void)unlink(g_path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind(%s): %s\n"
                "  (is wei still running? 'systemctl stop wei' first)\n",
                g_path, strerror(errno));
        close(fd);
        return 1;
    }

    printf("wei_sockdump: listening on %s  (Ctrl-C to stop, then restart wei)\n",
           g_path);
    fflush(stdout);

    while (!g_stop) {
        ssize_t r = recv(fd, buf, sizeof(buf), 0);
        char ts[16];
        unsigned type, ver, elem_size, len;
        size_t payload, count, i;

        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "recv: %s\n", strerror(errno));
            break;
        }
        ts_now(ts, sizeof(ts));

        if ((size_t)r < TLV_HDR_LEN) {
            printf("[%s] runt datagram: %zd bytes\n", ts, r);
            fflush(stdout);
            continue;
        }

        type      = buf[0];
        ver       = buf[1];
        elem_size = (unsigned)buf[2] | ((unsigned)buf[3] << 8);   /* host LE */
        len       = (unsigned)buf[4] | ((unsigned)buf[5] << 8);
        payload   = (size_t)r - TLV_HDR_LEN;
        count     = (elem_size > 0) ? (len / elem_size) : 0;

        printf("[%s] %zd bytes | type=%u(%s) ver=%u elem_size=%u len=%u -> count=%zu%s\n",
               ts, r, type, msg_type_str(type), ver, elem_size, len, count,
               (len != payload) ? "  [len != received payload!]" : "");

        for (i = 0; i < count; i++) {
            const uint8_t *entry = buf + TLV_HDR_LEN + i * elem_size;
            char mac[19];
            size_t j;

            /* mac_str is the first field of stats_arg_t (ASCII, NUL-terminated). */
            memcpy(mac, entry, sizeof(mac) - 1);
            mac[sizeof(mac) - 1] = '\0';
            for (j = 0; j < sizeof(mac) - 1; j++) {
                if (mac[j] == '\0') {
                    break;
                }
                if (mac[j] < 0x20 || mac[j] > 0x7e) {
                    mac[j] = '.';
                }
            }
            printf("  [%zu] mac=\"%s\"\n", i, mac);
            hex_preview(entry, elem_size);
        }
        fflush(stdout);
    }

    printf("\nwei_sockdump: stopping, unlinking %s -- now run 'systemctl start wei'\n",
           g_path);
    close(fd);
    (void)unlink(g_path);
    return 0;
}
