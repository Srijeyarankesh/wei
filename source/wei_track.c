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

#include "wei_track.h"

#include <string.h>

void wei_track_init(wei_track_table_t *table)
{
    if (table == NULL) {
        return;
    }

    memset(table, 0, sizeof(*table));
}

wei_track_entry_t *wei_track_get(wei_track_table_t *table, const uint8_t mac[6], int64_t now_ms)
{
    wei_track_entry_t *slot;
    size_t i;

    if (table == NULL || mac == NULL) {
        return NULL;
    }

    slot = NULL;
    for (i = 0; i < WEI_TRACK_MAX_CLIENTS; i++) {
        wei_track_entry_t *e = &table->entries[i];

        if (!e->in_use) {
            if (slot == NULL) {
                slot = e;
            }
            continue;
        }
        if (memcmp(e->mac, mac, sizeof(e->mac)) == 0) {
            e->last_seen = now_ms;
            return e;
        }
    }

    if (slot == NULL) {
        return NULL;
    }

    memset(slot, 0, sizeof(*slot));
    memcpy(slot->mac, mac, sizeof(slot->mac));
    slot->in_use = 1;
    slot->last_seen = now_ms;
    return slot;
}

void wei_track_reap(wei_track_table_t *table, int64_t now_ms, int64_t absence_ms)
{
    size_t i;

    if (table == NULL) {
        return;
    }

    for (i = 0; i < WEI_TRACK_MAX_CLIENTS; i++) {
        wei_track_entry_t *e = &table->entries[i];

        if (e->in_use && now_ms - e->last_seen > absence_ms) {
            memset(e, 0, sizeof(*e));
        }
    }
}

void wei_track_tick(wei_track_entry_t *e, int32_t metric, int64_t now_ms,
    int32_t band, int64_t floor_ms)
{
    if (e == NULL) {
        return;
    }

    if (metric < band) {
        if (e->start_ts == 0) {
            e->start_ts = now_ms;
            e->count++;
        }
        return;
    }

    if (e->start_ts != 0) {
        int64_t elapsed = now_ms - e->start_ts;

        if (elapsed >= floor_ms) {
            e->accumulated += elapsed;
        }
        e->start_ts = 0;
    }
}

void wei_track_collect(wei_track_entry_t *e, wei_track_record_t *out)
{
    if (e == NULL || out == NULL) {
        return;
    }

    out->count = e->count;
    out->duration_ms = e->accumulated;
    e->count = 0;
    e->accumulated = 0;
}
