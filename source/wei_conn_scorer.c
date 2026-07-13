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

#include "wei_conn_scorer.h"
#include "run_qmgr.h"

#include <math.h>

/* Standardised output scale endpoints (gate Q1): the final stage linearly
 * rescales the bounded [0,1] weighted value onto this 0-100 interpretable scale. */
#define CSCORE_SCALE_MIN   0.0
#define CSCORE_SCALE_MAX 100.0

double cscore_normalize_metric(double value, double lo, double hi)
{
    double span = hi - lo;
    double unit;

    if (span <= 0.0) {
        return 0.0;
    }

    unit = (value - lo) / span;
    if (unit < 0.0) {
        return 0.0;
    }
    if (unit > 1.0) {
        return 1.0;
    }
    return unit;
}

double cscore_rms_reduce(const double *metrics, size_t count)
{
    double accum = 0.0;
    size_t i;

    if (metrics == NULL || count == 0) {
        return 0.0;
    }

    for (i = 0; i < count; i++) {
        double m = metrics[i];
        accum += copysign(m * m, m); /* signed square: penalising metrics subtract */
    }

    accum /= (double)count;
    if (accum < 0.0) {
        return 0.0;
    }
    return sqrt(accum);
}

double cscore_chanutil_weight(double reduced, double chan_util)
{
    double exponent = -(LINK_QTY_B0 + LINK_QTY_B1 * chan_util);

    if (exponent < -50.0) {
        exponent = -50.0;
    }
    if (exponent > 50.0) {
        exponent = 50.0;
    }

    double weight = 1.0 / (1.0 + exp(exponent));

    return reduced * weight;
}

double cscore_standardize(double weighted)
{
    double scaled = CSCORE_SCALE_MIN + weighted * (CSCORE_SCALE_MAX - CSCORE_SCALE_MIN);

    if (scaled < CSCORE_SCALE_MIN) {
        return CSCORE_SCALE_MIN;
    }
    if (scaled > CSCORE_SCALE_MAX) {
        return CSCORE_SCALE_MAX;
    }
    return scaled;
}

uint8_t wei_conn_scorer_score(const wei_conn_metric_record_t *record)
{
    double norm[3];
    double reduced;
    double weighted;

    if (record == NULL ||
        record->activity_state != WEI_CONN_METRIC_STATE_ACTIVE ||
        record->status != WEI_CONN_METRIC_STATUS_OK) {
        return 0;
    }

    norm[0] =  cscore_normalize_metric((double)record->link_snr_db,   0.0,      40.0);
    norm[1] =  cscore_normalize_metric((double)record->phy_rate_kbps, 0.0, 866000.0);
    /* PER is a penalty: it enters negative so the signed-square RMS subtracts it. */
    norm[2] = -cscore_normalize_metric((double)record->pkt_err_rate,  0.0,       1.0);

    reduced  = cscore_rms_reduce(norm, 3);
    weighted = cscore_chanutil_weight(reduced, (double)record->chan_util_pct);

    return (uint8_t)lround(cscore_standardize(weighted));
}
