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

#include "weid_bus.h"
#include "wei_perfreport.h"
#include "bus.h"
#include "weid_rfc.h"

#include <cjson/cJSON.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Per-client cadence state behind the edge-triggered emit: last_emitted_class is
 * the C4 verdict class of this client's most recently published report, the
 * baseline the class-transition edge is measured against, and warmup_ticks_seen
 * counts the reporting intervals observed before this client's first publish so a
 * warm-up ramp can suppress early transient churn. in_use marks a live slot; a
 * slot carries no timestamp because this unit admits but does not reap within a
 * session. Heap-free and bounded: the table is ceilinged at the same per-station
 * capacity the scorer tracks and lives entirely in .bss. */
typedef struct {
    uint8_t             mac[6];
    uint8_t             in_use;
    wei_infer_verdict_t last_emitted_class;
    uint16_t            warmup_ticks_seen;
} wei_perfreport_client_state_t;

static wei_perfreport_client_state_t wei_perfreport_state_tbl[WEI_TRACK_MAX_CLIENTS];

/* Resolves this client's cadence slot, admitting a zero-seeded one on first sight.
 * Mirrors the wei_track membership scan: returns the live slot on a MAC match,
 * else claims the first free slot for the key, else returns NULL when the key is
 * absent and the fixed table is full -- a defined not-found result the caller
 * skips the interval on, never an out-of-bounds access. */
static wei_perfreport_client_state_t *wei_perfreport_state_find_or_add(const uint8_t client_mac[6])
{
    wei_perfreport_client_state_t *free_slot;
    unsigned int i;

    if (client_mac == NULL) {
        return NULL;
    }

    free_slot = NULL;
    for (i = 0; i < WEI_TRACK_MAX_CLIENTS; i++) {
        wei_perfreport_client_state_t *s = &wei_perfreport_state_tbl[i];

        if (!s->in_use) {
            if (free_slot == NULL) {
                free_slot = s;
            }
            continue;
        }
        if (memcmp(s->mac, client_mac, sizeof(s->mac)) == 0) {
            return s;
        }
    }

    if (free_slot == NULL) {
        return NULL;
    }

    memset(free_slot, 0, sizeof(*free_slot));
    memcpy(free_slot->mac, client_mac, sizeof(free_slot->mac));
    free_slot->in_use = 1;
    return free_slot;
}

/* Compact single-document report, so a client's emit stays small and bounded; the
 * reused buffer opens at INIT and grows on demand only up to MAX. */
#define WEI_PERFREPORT_JSON_BUF_INIT 512u
#define WEI_PERFREPORT_JSON_BUF_MAX  2048u

/* One serialization buffer reused across every emit: allocated once, grown on
 * demand up to WEI_PERFREPORT_JSON_BUF_MAX and never freed per tick, so a steady-
 * state report costs no allocation. Owned by the single per-interval publish
 * context (single-writer), so it carries no lock. */
static char  *wei_perfreport_json_buf;
static size_t wei_perfreport_json_buf_cap;

static const char *wei_perfreport_verdict_name(wei_infer_verdict_t verdict)
{
    switch (verdict) {
    case WEI_INFER_VERDICT_SMOOTH:   return "smooth";
    case WEI_INFER_VERDICT_DEGRADED: return "degraded";
    default:                         return "unknown";
    }
}

static const char *wei_perfreport_contributor_name(wei_infer_contributor_t contributor)
{
    switch (contributor) {
    case WEI_INFER_CONTRIB_NONE:      return "none";
    case WEI_INFER_CONTRIB_SNR:       return "snr";
    case WEI_INFER_CONTRIB_PHY:       return "phy_rate";
    case WEI_INFER_CONTRIB_PKT_ERR:   return "pkt_err";
    case WEI_INFER_CONTRIB_CHAN_UTIL: return "chan_util";
    default:                          return "unknown";
    }
}

/* Serializes doc into the reused buffer, growing it on demand within the bound.
 * Returns the buffer on success, NULL when the document exceeds the ceiling or an
 * allocation fails -- on which the prior buffer is preserved intact (no leak). */
static const char *wei_perfreport_json_serialize(cJSON *doc)
{
    for (;;) {
        char *grown;
        size_t next;

        if (wei_perfreport_json_buf == NULL) {
            wei_perfreport_json_buf = (char *)malloc(WEI_PERFREPORT_JSON_BUF_INIT);
            if (wei_perfreport_json_buf == NULL) {
                return NULL;
            }
            wei_perfreport_json_buf_cap = WEI_PERFREPORT_JSON_BUF_INIT;
        }

        if (cJSON_PrintPreallocated(doc, wei_perfreport_json_buf,
                (int)wei_perfreport_json_buf_cap, 0)) {
            return wei_perfreport_json_buf;
        }

        if (wei_perfreport_json_buf_cap >= WEI_PERFREPORT_JSON_BUF_MAX) {
            return NULL;
        }
        next = wei_perfreport_json_buf_cap * 2u;
        if (next > WEI_PERFREPORT_JSON_BUF_MAX) {
            next = WEI_PERFREPORT_JSON_BUF_MAX;
        }
        grown = (char *)realloc(wei_perfreport_json_buf, next);
        if (grown == NULL) {
            return NULL;
        }
        wei_perfreport_json_buf = grown;
        wei_perfreport_json_buf_cap = next;
    }
}

/* Marshals one scored client's report -- stable MAC key, report stamp, standardised
 * score, categorical verdict and dominant contributor, and the contributing C1
 * metric breakdown the score derived from -- into a cJSON document built directly
 * (no webconfig subdoc machinery) and serialized compactly into the reused buffer.
 * result and metrics are read read-only and never written. Returns the serialized
 * string, or NULL on a NULL input, a cJSON create failure, or a serialize failure;
 * the cJSON tree is deleted exactly once on every path. */
static const char *wei_perfreport_build_json(const uint8_t client_mac[6],
    const wei_infer_result_t *result, const wei_conn_metric_record_t *metrics)
{
    const char *json;
    cJSON *doc;
    char mac_str[18];
    char stamp[128];

    if (client_mac == NULL || result == NULL || metrics == NULL) {
        return NULL;
    }

    doc = cJSON_CreateObject();
    if (doc == NULL) {
        return NULL;
    }

    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
        client_mac[0], client_mac[1], client_mac[2],
        client_mac[3], client_mac[4], client_mac[5]);

    cJSON_AddStringToObject(doc, "client_mac", mac_str);
    cJSON_AddStringToObject(doc, "report_time", get_formatted_time(stamp));
    cJSON_AddNumberToObject(doc, "score", result->score);
    cJSON_AddStringToObject(doc, "verdict", wei_perfreport_verdict_name(result->verdict));
    cJSON_AddStringToObject(doc, "dominant", wei_perfreport_contributor_name(result->dominant));
    cJSON_AddNumberToObject(doc, "snr_db", metrics->link_snr_db);
    cJSON_AddNumberToObject(doc, "phy_rate_kbps", metrics->phy_rate_kbps);
    cJSON_AddNumberToObject(doc, "pkt_err_rate", metrics->pkt_err_rate);
    cJSON_AddNumberToObject(doc, "chan_util_pct", metrics->chan_util_pct);
    cJSON_AddNumberToObject(doc, "activity_state", metrics->activity_state);
    cJSON_AddNumberToObject(doc, "status", metrics->status);

    json = wei_perfreport_json_serialize(doc);
    cJSON_Delete(doc);
    doc = NULL;
    return json;
}

/* Warm-up ramp: a client's first N reporting intervals are suppressed before its
 * first publish, damping the transient verdict churn a freshly-observed client
 * shows while its normalisation bands are still settling. */
#define WEI_PERFREPORT_WARMUP_INTERVALS 3u

/* Composed edge-triggered emit gate: admits a client's report only when the
 * connected-performance pillar is enabled, the client's warm-up ramp has elapsed,
 * and its serviceable-state verdict has changed since its last published report --
 * the transition filter that keeps steady-state clients off the bus. The pillar
 * enable is read through the daemon RFC accessor, the same enable-of-record C6's
 * own accessors consult, so the standalone daemon carries no cross-process config
 * dependency. A disabled pillar short-circuits before the warm-up counter advances,
 * so a client burns no ramp while the feature is off. Mutates only warmup_ticks_seen;
 * last_emitted_class is committed by the caller after a completed emit. */
static bool wei_perfreport_should_emit(wei_perfreport_client_state_t *state,
    wei_infer_verdict_t current_class)
{
    if (state == NULL) {
        return false;
    }

    if (!weid_rfc_connperf_enabled()) {
        return false;
    }

    if (state->warmup_ticks_seen < WEI_PERFREPORT_WARMUP_INTERVALS) {
        state->warmup_ticks_seen++;
        return false;
    }

    return current_class != state->last_emitted_class;
}

/* Per-interval publish for one scored client: on gate admission it marshals the
 * report and fires exactly one event through C6's WEI_CONNPERF_DM_REPORT_EVENT in
 * the raw_data_t shape events_bus_publish emits -- bus_data_type_string, length
 * including the NUL. Reached only from the single wei inference tick, so the reused
 * buffer and state table stay lock-free and no lock is held across the bus call.
 * The published class is committed as the edge baseline only after a successful
 * publish, so a failed emit leaves the per-client state intact for the next
 * interval to retry. */
bus_error_t wei_perfreport_publish_tick(const uint8_t client_mac[6],
    const wei_infer_result_t *result, const wei_conn_metric_record_t *metrics)
{
    wei_perfreport_client_state_t *state;
    wifi_bus_desc_t *desc;
    bus_handle_t *ctrl;
    const char *json;
    raw_data_t data;
    bus_error_t rc;

    if (client_mac == NULL || result == NULL || metrics == NULL) {
        return bus_error_invalid_input;
    }

    state = wei_perfreport_state_find_or_add(client_mac);
    if (state == NULL) {
        return bus_error_success;
    }

    if (!wei_perfreport_should_emit(state, result->verdict)) {
        return bus_error_success;
    }

    ctrl = weid_bus_handle();
    desc = get_bus_descriptor();
    if (ctrl == NULL || desc == NULL || desc->bus_event_publish_fn == NULL) {
        return bus_error_general;
    }

    json = wei_perfreport_build_json(client_mac, result, metrics);
    if (json == NULL) {
        return bus_error_general;
    }

    memset(&data, 0, sizeof(data));
    data.data_type = bus_data_type_string;
    data.raw_data.bytes = (void *)json;
    data.raw_data_len = (unsigned int)(strlen(json) + 1);

    rc = desc->bus_event_publish_fn(ctrl, WEI_CONNPERF_DM_REPORT_EVENT, &data);
    if (rc != bus_error_success) {
        return rc;
    }

    state->last_emitted_class = result->verdict;
    return bus_error_success;
}
