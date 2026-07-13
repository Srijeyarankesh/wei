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

#include "weid_rfc.h"
#include "weid_bus.h"
#include "weid_wire.h"

#include <malloc.h>
#include <pthread.h>
#include <signal.h>

/* Keep the daemon's small allocations off mmap and let the heap trim back down,
 * so steady-state RSS stays bounded on the gateway; ~64 KB per the real
 * wei_main.cpp shape. */
#define WEID_MALLOC_THRESHOLD (64 * 1024)

/* The blocking receive/tick body runs on its own thread so main can rest in
 * sigwait. wei_poll_run loops until the thread is cancelled at its poll()
 * cancellation point -- the only stop the committed C3 loop offers -- after
 * which main tears the pipeline down with no thread still touching it. */
static void *weid_daemon_body(void *arg)
{
    weid_ctx_t *ctx = (weid_ctx_t *)arg;

    wei_poll_run(ctx->poll);
    return NULL;
}

int main(void)
{
    sigset_t set;
    pthread_t body;
    weid_ctx_t ctx = { 0 };
    int sig = 0;

    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) {
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);

    mallopt(M_MMAP_THRESHOLD, WEID_MALLOC_THRESHOLD);
    mallopt(M_TRIM_THRESHOLD, WEID_MALLOC_THRESHOLD);

    weid_rfc_load();

    if (weid_bus_open() != bus_error_success) {
        return 1;
    }

    if (weid_wire_init(&ctx) != 0) {
        weid_bus_close();
        return 1;
    }

    if (pthread_create(&body, NULL, weid_daemon_body, &ctx) != 0) {
        weid_wire_deinit(&ctx);
        weid_bus_close();
        return 1;
    }

    sigwait(&set, &sig);

    pthread_cancel(body);
    pthread_join(body, NULL);

    weid_wire_deinit(&ctx);
    weid_bus_close();

    return 0;
}
