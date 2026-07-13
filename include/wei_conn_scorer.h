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

#ifndef WEI_CONN_SCORER_H
#define WEI_CONN_SCORER_H
#ifdef __cplusplus
extern "C" {
#endif

#include "wei_conn_metric.h"
#include <stddef.h>

/* Connected-P scoring kernel: reduces one client's C1 metric record to a single
 * standardised connected-P scalar. Pillar-independent by construction (CP-6) --
 * the unit includes no other-pillar header and reads only the C1 per-link metric
 * vector plus its own internal constants; any additional input is a C1 contract
 * change surfaced upstream, never a cross-pillar read here. Pure synchronous
 * leaf: it owns no loop, map, timer, thread, lock, daemon, bus event, accessor,
 * or enable state, and is invoked once per client by the C4 sweep. */

/* Public per-client kernel entry: reduces one C1 connected-P metric record to a
 * single standardised 0-100 score (higher = better). Gates on activity/status --
 * a non-active or non-OK record is suppressed and returns 0 -- then runs the
 * fixed pipeline normalise -> signed-square RMS -> channel-utilisation sigmoid
 * weight -> standardise. Pure and re-entrant over its one const record: it
 * mutates nothing and owns no state, and is called once per connected client
 * by the C4 sweep. */
uint8_t wei_conn_scorer_score(const wei_conn_metric_record_t *record);

/* Normalises one raw link metric onto [0,1] via a clamped min-max map -- the
 * single shared normalisation path bound by name by CH-1 and CP-12 (input half
 * of the one normalize-to-standard-scale convention). The caller supplies the
 * metric's [lo,hi] domain; a non-positive span yields 0.0. No allocation, no lock. */
double cscore_normalize_metric(double value, double lo, double hi);

/* Reduces the normalised link-metric vector to one bounded intermediate scalar via
 * signed-square RMS -- sqrt(mean(±metric²)), each element's sign carrying its metric
 * direction so a penalising metric (arriving negative) subtracts. A net-negative mean
 * or an empty vector yields 0.0. The single reduction bound by name by CH-1 and CP-6,
 * not one per score variant. No allocation, no lock. */
double cscore_rms_reduce(const double *metrics, size_t count);

/* Weights the RMS-reduced scalar by a logistic function of channel utilisation --
 * reduced × sigmoid(LINK_QTY_B0 + LINK_QTY_B1·chan_util) -- applied before the
 * standardised-scale transform. chan_util is a raw 0-100 channel-utilisation percent.
 * The coefficients are consumed by name from Sanjay's run_qmgr.h (LINK_QTY_B0 /
 * LINK_QTY_B1), not local constants, so the weight follows real wei's sigmoid and
 * rises with utilisation rather than the prior inverted local curve. Direct
 * transcendental, no allocation, no lock. */
double cscore_chanutil_weight(double reduced, double chan_util);

/* Maps the bounded [0,1] weighted value onto the single standardised 0-100
 * interpretable scale as the kernel's final stage -- the output half of the one
 * normalize-to-standard-scale convention (gate Q1), bound by name by CH-1 and
 * CP-12. The reference [0,1] output is not carried forward. C5 emits the scalar
 * only; interpretation banding is downstream policy (gate Q2). No allocation, no lock. */
double cscore_standardize(double weighted);

#ifdef __cplusplus
}
#endif
#endif /* WEI_CONN_SCORER_H */
