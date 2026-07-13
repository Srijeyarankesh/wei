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

#ifndef WEI_PERFREPORT_H
#define WEI_PERFREPORT_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Consumed contracts, bound by their exact declared names and re-authored by
 * none of this unit: the C4 per-client inference result (standardised score and
 * serviceable-state verdict) read read-only; the C1 per-client metric record the
 * contributing signal/PHY/error/utilisation values are read read-only from; and
 * C6's data-model surface carrying WEI_CONNPERF_DM_REPORT_EVENT this unit fires
 * through, which pulls in the platform bus abstraction whose bus_error_t it
 * returns. This unit registers no element and defines no enable. */
#include "wei_infer.h"
#include "wei_conn_metric.h"
#include "wei_connperf_dml.h"

/* Per-reporting-interval publish entry, invoked once per scored client on the wei
 * inference tick: gates on the connected-performance pillar enable and the
 * edge-triggered warm-up cadence, marshals the client's key, score, verdict,
 * contributing metrics and report stamp into the report payload, and fires
 * exactly one event through C6's already-registered WEI_CONNPERF_DM_REPORT_EVENT.
 * client_mac is the stable 6-byte key; result and metrics are read-only and never
 * written. NULL-safe: returns a defined bus_error_t on every path. */
bus_error_t wei_perfreport_publish_tick(const uint8_t client_mac[6],
    const wei_infer_result_t *result, const wei_conn_metric_record_t *metrics);

#ifdef __cplusplus
}
#endif
#endif /* WEI_PERFREPORT_H */
