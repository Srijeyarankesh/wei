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

#ifndef WEI_POLL_H
#define WEI_POLL_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#define WEI_TICK_DEFAULT_MS 5000

typedef struct wei_poll_ctx wei_poll_ctx_t;

/* buf is borrowed for the call only; copy it to retain past return. */
typedef void (*wei_poll_on_datagram)(const uint8_t *buf, size_t len, void *user);
typedef void (*wei_poll_on_tick)(void *user);

wei_poll_ctx_t *wei_poll_create(const char *sock_path, wei_poll_on_datagram on_datagram,
    wei_poll_on_tick on_tick, void *user);
int wei_poll_run(wei_poll_ctx_t *ctx);
/* Change the tick period at runtime, effective from the next tick without
 * restarting the loop or dropping pending datagrams; period_ms must be > 0. */
int wei_poll_retune(wei_poll_ctx_t *ctx, unsigned int period_ms);
void wei_poll_stop(wei_poll_ctx_t *ctx);

#define WEI_POLL_ROLE_MAX 8

/* Resolves the C1 TLV role of a drained datagram to a handler slot in
 * [0, WEI_POLL_ROLE_MAX), or a negative value to discard it. The caller owns
 * the concrete C1 wire binding; this layer defines no TLV layout. */
typedef int (*wei_poll_role_of)(const uint8_t *buf, size_t len, void *user);

typedef struct {
    wei_poll_role_of role_of;
    wei_poll_on_datagram handler[WEI_POLL_ROLE_MAX];
    void *user;
} wei_poll_dispatch_t;

/* Routes a drained datagram to the handler registered for its role; an unknown
 * or unregistered role is discarded so the poll loop keeps draining. */
void wei_poll_dispatch(const wei_poll_dispatch_t *d, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* WEI_POLL_H */
