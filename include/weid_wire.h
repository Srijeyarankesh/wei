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

#ifndef WEID_WIRE_H
#define WEID_WIRE_H
#ifdef __cplusplus
extern "C" {
#endif

/* Consumed by their exact declared names, redefined by neither: the C4 scorer
 * engine driven per record and per tick, and the C3 single poll loop that owns
 * the one AF_UNIX socket. */
#include "wei_infer.h"
#include "wei_poll.h"

/* End-to-end wiring context, owned by the daemon for its whole lifetime: the one
 * poll loop bound to the linkquality socket and the scorer engine it feeds. The
 * datagram callback stages each parsed record into the engine; the tick callback
 * drives the engine's per-interval scoring and report. */
typedef struct {
    wei_poll_ctx_t     *poll;
    wei_infer_engine_t *engine;
} weid_ctx_t;

/* Builds the pipeline: creates the scorer engine (which owns no poll) and the one
 * poll loop bound to LQ_STATS_SOCKET_PATH at the 5 s tick cadence, registering the
 * datagram and tick callbacks against ctx. Returns 0 on success; on failure frees
 * anything partially created and returns -1. */
int weid_wire_init(weid_ctx_t *ctx);

/* Tears the pipeline down once: stops and frees the poll loop, then the engine.
 * NULL-safe and idempotent. */
void weid_wire_deinit(weid_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
#endif /* WEID_WIRE_H */
