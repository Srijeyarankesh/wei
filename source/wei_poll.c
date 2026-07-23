/**
 * Copyright 2026 Comcast Cable Communications Management, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wei_poll.h"
#include "wei_util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* A single datagram must hold the linkquality sender's largest legal frame.
 * lq_ipc_sender.c build_tlv() prepends a fixed 6-byte header and caps the value
 * at UINT16_MAX bytes, so header + UINT16_MAX is the most it can ever emit.
 * Sizing the recv() buffer to that maximum makes per-datagram truncation
 * impossible for any client count. The previous 2048 truncated even a single
 * count=1 stats frame (6-byte header + sizeof(stats_arg_t)=3776 = 3782 bytes),
 * which the receiver then rejected as "tlv.len != payload". */
#define WEI_DGRAM_MAX (UINT16_MAX + 64u)

struct wei_poll_ctx {
    int fd;
    unsigned int tick_ms;
    wei_poll_on_datagram on_datagram;
    wei_poll_on_tick on_tick;
    void *user;
    char sock_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

static int wei_sock_open(const char *sock_path)
{
    struct sockaddr_un addr;
    int fd;

    if (sock_path == NULL || strlen(sock_path) >= sizeof(addr.sun_path)) {
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }

    /* Match the OneWifi linkquality sender's 4 MB SO_SNDBUF so 50+ client
     * bursts are queued in the socket rather than dropped by the kernel.
     * Best-effort: ignore failure. */
    {
        int rcvbuf = 4 * 1024 * 1024;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    unlink(sock_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void wei_sock_drain(wei_poll_ctx_t *ctx)
{
    /* Drained only from the single poll thread, one datagram fully consumed per
     * iteration, so a static buffer is safe and keeps this ~64 KiB frame off the
     * stack. */
    static uint8_t buf[WEI_DGRAM_MAX];
    ssize_t len;

    for (;;) {
        len = recv(ctx->fd, buf, sizeof(buf), 0);
        if (len < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (len > 0 && ctx->on_datagram != NULL) {
            wei_util_dbg_print(WEI_CONNECTED, "%s:%d [IPC-RECV] datagram=%zd bytes\n",
                __func__, __LINE__, len);
            ctx->on_datagram(buf, (size_t)len, ctx->user);
        }
    }
}

static int64_t wei_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

wei_poll_ctx_t *wei_poll_create(const char *sock_path, wei_poll_on_datagram on_datagram,
    wei_poll_on_tick on_tick, void *user)
{
    wei_poll_ctx_t *ctx;
    int fd;

    fd = wei_sock_open(sock_path);
    if (fd < 0) {
        return NULL;
    }

    ctx = (wei_poll_ctx_t *)calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        close(fd);
        return NULL;
    }

    ctx->fd = fd;
    ctx->tick_ms = WEI_TICK_DEFAULT_MS;
    ctx->on_datagram = on_datagram;
    ctx->on_tick = on_tick;
    ctx->user = user;
    strncpy(ctx->sock_path, sock_path, sizeof(ctx->sock_path) - 1);
    ctx->sock_path[sizeof(ctx->sock_path) - 1] = '\0';

    return ctx;
}

/* Re-establish the endpoint when its bound path has vanished or is no longer a
 * socket; a healthy path is left untouched. On failure the fd is left negative
 * so poll() skips it and the next tick retries rather than aborting the loop. */
static int wei_poll_revive_socket(wei_poll_ctx_t *ctx)
{
    struct stat st;
    int fd;

    if (ctx == NULL) {
        return -1;
    }

    if (stat(ctx->sock_path, &st) == 0 && S_ISSOCK(st.st_mode)) {
        return 0;
    }

    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }

    fd = wei_sock_open(ctx->sock_path);
    if (fd < 0) {
        return -1;
    }

    ctx->fd = fd;
    return 0;
}

int wei_poll_run(wei_poll_ctx_t *ctx)
{
    struct pollfd pfd;
    int64_t next_tick, now, wait;
    int rc;

    if (ctx == NULL) {
        return -1;
    }

    pfd.events = POLLIN;
    next_tick = wei_now_ms() + ctx->tick_ms;

    for (;;) {
        now = wei_now_ms();
        wait = next_tick - now;
        if (wait < 0) {
            wait = 0;
        }

        pfd.fd = ctx->fd;
        pfd.revents = 0;
        rc = poll(&pfd, 1, (int)wait);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return -1;
        }

        if (pfd.revents & POLLIN) {
            wei_sock_drain(ctx);
        }

        now = wei_now_ms();
        if (now >= next_tick) {
            wei_poll_revive_socket(ctx);
            if (ctx->on_tick != NULL) {
                ctx->on_tick(ctx->user);
            }
            next_tick = now + ctx->tick_ms;
        }
    }
}

int wei_poll_retune(wei_poll_ctx_t *ctx, unsigned int period_ms)
{
    if (ctx == NULL || period_ms == 0) {
        return -1;
    }

    ctx->tick_ms = period_ms;
    return 0;
}

void wei_poll_stop(wei_poll_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->fd >= 0) {
        close(ctx->fd);
    }

    free(ctx);
}

void wei_poll_dispatch(const wei_poll_dispatch_t *d, const uint8_t *buf, size_t len)
{
    int role;

    if (d == NULL || d->role_of == NULL) {
        return;
    }

    role = d->role_of(buf, len, d->user);
    if (role < 0 || role >= WEI_POLL_ROLE_MAX) {
        return;
    }

    if (d->handler[role] != NULL) {
        d->handler[role](buf, len, d->user);
    }
}
