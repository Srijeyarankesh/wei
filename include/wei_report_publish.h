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

#ifndef WEI_REPORT_PUBLISH_H
#define WEI_REPORT_PUBLISH_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Consumed contracts bound by their exact declared names: the C4 per-client
 * inference result this unit reads read-only for the standardised score and the
 * serviceable-state verdict, and the platform bus abstraction whose bus_error_t
 * this unit returns. This unit redefines neither. */
#include "wei_infer.h"
#include "bus_common.h"

/* Fixed-size per-client transport record marshalled once per reporting interval
 * and handed to the bus: the stable MAC key downstream joins to customer-contact
 * data, the standardised 0-100 connected-performance score and its smooth-to-
 * degraded verdict read from the C4 result, and the emit-time report stamp in
 * epoch milliseconds. No pointer-to-heap member, so it marshals into a bounded
 * buffer with no hot-path allocation. */
typedef struct {
    uint8_t             client_mac[6];
    uint8_t             score;
    wei_infer_verdict_t verdict;
    int64_t             report_ts_ms;
} wei_report_record_t;

/* Single per-reporting-interval publish entry: gates on the connected-performance
 * pillar enable, marshals one client's (key, score, verdict, stamp) from the
 * read-only C4 result, and fires exactly one event through the already-registered
 * report element. The MAC key is passed alongside because the C4 result carries
 * the score and verdict only. Total and crash-safe: NULL-checks its pointer
 * argument and returns a defined bus_error_t on every path. */
bus_error_t wei_report_publish_tick(const uint8_t client_mac[6],
    const wei_infer_result_t *result);

#ifdef __cplusplus
}
#endif
#endif /* WEI_REPORT_PUBLISH_H */
