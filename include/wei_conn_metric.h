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

#ifndef WEI_CONN_METRIC_H
#define WEI_CONN_METRIC_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Single contract owning the per-client connected-P metric shape: C3 drains,
 * C4 maps, C5 scores, C6 serialises, C7/C8 report -- all by name. Redefinition
 * of the record in any consumer is a defect. Same-box loopback, host byte order
 * (gate Q1). */

/* Bumped on any change to the record layout so C6 (sender) and C3 (receiver)
 * can reject a shape they do not recognise. Fronts every record. */
#define WEI_CONN_METRIC_VERSION 3u

/* The record's leading field is a single version byte, first in wire order. */
typedef uint8_t wei_conn_metric_ver_t;

/* Client activity state gating whether C5 scores this client (CH-5): a
 * non-active client is carried with its state so scoring can be suppressed
 * or qualified. Shape transposed from client_state_t in wifi_base.h with
 * fresh enumerators -- no identifier reuse. Three states per gate Q4. */
typedef enum {
    WEI_CONN_METRIC_STATE_ACTIVE   = 0,
    WEI_CONN_METRIC_STATE_IDLE     = 1,
    WEI_CONN_METRIC_STATE_SLEEPING = 2
} wei_conn_metric_state_t;

/* Status/result code stating whether a record's metrics are usable: C5 keys
 * scoring on it, C8 reports it. Absent or stale data is signalled here, never
 * faked with a fabricated metric, so the scorer can suppress a client cleanly.
 * Seed set is extendable -- append new codes, never redefine in a consumer
 * (gate Q2). */
typedef enum {
    WEI_CONN_METRIC_STATUS_OK               = 0,
    WEI_CONN_METRIC_STATUS_NO_CLIENT_DATA   = 1,
    WEI_CONN_METRIC_STATUS_STALE            = 2,
    WEI_CONN_METRIC_STATUS_SCORE_SUPPRESSED = 3
} wei_conn_metric_status_t;

/* The one per-client connected-P metric record: a fixed-size value copy that C3
 * drains, C4 maps, C5 normalises and C6 serialises by name. Filled under the
 * existing stats lock at the collect seam -- no heap, no new thread or lock.
 * Link-quality fields are transposed from sta_data_t.dev_stats (SNR / PHY rate)
 * with fresh names -- no identifier reuse. The version byte fronts the record;
 * naturally-aligned fixed-width fields, host byte order, same-box only (gate Q1),
 * so sizeof is a compile-time constant fitting one datagram. */
/* Scope: each record describes exactly one gateway-attached client. Clients
 * behind XE2+ extenders are excluded from this demo (intent Q2) -- the record
 * carries no extender/backhaul field and reserves no speculative slot for one
 * (CP-2). */
typedef struct {
    wei_conn_metric_ver_t    version;
    wei_conn_metric_state_t  activity_state;
    wei_conn_metric_status_t status;
    int32_t                  link_snr_db;
    uint32_t                 phy_rate_kbps;
    uint32_t                 tx_frames;      /* cumulative frames sent to this client: windowed-loss denominator base */
    uint32_t                 tx_err_frames;  /* cumulative send-error frames to this client: windowed-loss numerator base */
    uint8_t                  chan_util_pct;  /* channel utilization %, 0-100; C5 sigmoid de-weight input */
} wei_conn_metric_record_t;

#ifdef __cplusplus
}
#endif
#endif /* WEI_CONN_METRIC_H */
