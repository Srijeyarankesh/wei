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

#ifndef WEI_TRACK_H
#define WEI_TRACK_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#define WEI_TRACK_MAX_CLIENTS 64
#define WEI_TRACK_ABSENCE_DEFAULT_MS 30000

typedef struct {
    uint8_t mac[6];
    uint8_t in_use;
    int64_t last_seen;
    int64_t start_ts;
    int64_t accumulated;
    uint32_t count;
} wei_track_entry_t;

typedef struct {
    wei_track_entry_t entries[WEI_TRACK_MAX_CLIENTS];
} wei_track_table_t;

typedef struct {
    uint32_t count;
    int64_t duration_ms;
} wei_track_record_t;

void wei_track_init(wei_track_table_t *table);
/* Returns the client's ledger entry, claiming a free slot on first sight and
 * stamping last_seen; NULL when the fixed table is full. */
wei_track_entry_t *wei_track_get(wei_track_table_t *table, const uint8_t mac[6], int64_t now_ms);
/* Reclaims entries unseen for longer than absence_ms; no allocation. */
void wei_track_reap(wei_track_table_t *table, int64_t now_ms, int64_t absence_ms);
/* Advances one client's excursion state at now_ms: a metric below band opens an
 * excursion (counted once at onset); back within band closes it, accumulating
 * only excursions lasting at least floor_ms so brief blips are excluded. band and
 * floor_ms are caller-supplied policy inputs. O(1), no allocation. */
void wei_track_tick(wei_track_entry_t *e, int32_t metric, int64_t now_ms,
    int32_t band, int64_t floor_ms);
/* Yields the interval's {onset count, sustained duration} into *out and resets
 * both for the next interval; an in-progress excursion keeps timing across it. */
void wei_track_collect(wei_track_entry_t *e, wei_track_record_t *out);

#ifdef __cplusplus
}
#endif
#endif /* WEI_TRACK_H */
