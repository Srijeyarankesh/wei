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
#include "wei_report_publish.h"
#include "wei_connperf_dml.h"
#include "bus.h"

#include <stddef.h>
#include <string.h>
#include <time.h>

/* C6 owns this pillar-enable GET accessor in wei_connperf_dml.c but does not
 * export it; its prototype is mirrored here to gate on the enable by exact name. */
bus_error_t wei_connperf_enable_get(char *name, raw_data_t *p_data, bus_user_data_t *user_data);

/* Gate truth for every publish. Any accessor failure is treated as disabled so a
 * transient config gap emits nothing rather than spurious reports. */
static bool wei_report_enabled(void)
{
    raw_data_t data = { 0 };

    if (wei_connperf_enable_get(WEI_CONNPERF_DM_ENABLE, &data, NULL) != bus_error_success) {
        return false;
    }
    if (data.data_type != bus_data_type_boolean) {
        return false;
    }
    return data.raw_data.b;
}

/* Stamps the record's report time at emit. Unlike the wei app's interval clocks,
 * a reported-at stamp must be wall-clock epoch so downstream can join the record
 * to real time (CP-9), so this reads CLOCK_REALTIME rather than CLOCK_MONOTONIC. */
static void wei_report_stamp_time(wei_report_record_t *rec)
{
    struct timespec ts;

    if (rec == NULL) {
        return;
    }
    clock_gettime(CLOCK_REALTIME, &ts);
    rec->report_ts_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Marshals one client's (key, score, verdict, stamp) into the fixed-size record
 * and points the bus payload at it. The record is caller-owned, so the whole
 * marshal is stack-bounded with no hot-path allocation. data_type is set to the
 * bytes discriminator before the matching union member is written so the fire
 * path reads the union member the type advertises. The C4 result is read-only. */
static bus_error_t wei_report_marshal(const uint8_t client_mac[6],
    const wei_infer_result_t *result, wei_report_record_t *rec, raw_data_t *out)
{
    if (client_mac == NULL || result == NULL || rec == NULL || out == NULL) {
        return bus_error_invalid_input;
    }

    memcpy(rec->client_mac, client_mac, sizeof(rec->client_mac));
    rec->score = result->score;
    rec->verdict = result->verdict;
    wei_report_stamp_time(rec);

    memset(out, 0, sizeof(*out));
    out->data_type = bus_data_type_bytes;
    out->raw_data.bytes = rec;
    out->raw_data_len = sizeof(*rec);

    return bus_error_success;
}

/* Fires one marshalled report through C6's registered event element; this unit
 * registers nothing. The caller passes the opened bus handle because the shared
 * ctrl handle lives in the tick's context, keeping this helper free of the core
 * ctrl header. Mirroring the daemon bus_register_handlers guard, the descriptor
 * and its publish member are both NULL-checked before the call so an rbus-time
 * NULL cannot be dereferenced. */
static bus_error_t wei_report_fire(bus_handle_t *handle, raw_data_t *out)
{
    bus_error_t rc;

    if (handle == NULL || out == NULL) {
        return bus_error_invalid_input;
    }

    if (get_bus_descriptor() == NULL || get_bus_descriptor()->bus_event_publish_fn == NULL) {
        return bus_error_general;
    }

    rc = get_bus_descriptor()->bus_event_publish_fn(handle, WEI_CONNPERF_DM_REPORT_EVENT, out);
    if (rc != bus_error_success) {
        return rc;
    }

    return bus_error_success;
}

/* Reduced-frequency throttle seam (CP-11, MAY): the single point where a future
 * operator-chosen change-magnitude or background-activity threshold attaches, so
 * enabling throttling stays a local change with no caller rework. The threshold
 * is undefined, so this first cut admits every interval. Reads nothing from the
 * result, so it is inherently NULL-safe. */
static bool wei_report_should_emit(const wei_infer_result_t *result)
{
    (void)result;
    return true;
}

bus_error_t wei_report_publish_tick(const uint8_t client_mac[6],
    const wei_infer_result_t *result)
{
    wei_report_record_t rec = { 0 };
    raw_data_t payload;
    bus_handle_t *ctrl;
    bus_error_t rc;

    if (client_mac == NULL || result == NULL) {
        return bus_error_invalid_input;
    }

    if (!wei_report_enabled()) {
        return bus_error_success;
    }

    if (!wei_report_should_emit(result)) {
        return bus_error_success;
    }

    ctrl = weid_bus_handle();
    if (ctrl == NULL) {
        return bus_error_general;
    }

    rc = wei_report_marshal(client_mac, result, &rec, &payload);
    if (rc != bus_error_success) {
        return rc;
    }

    return wei_report_fire(ctrl, &payload);
}
