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

#include "wei_life.h"
#include "wei_conn_scorer.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

struct wei_life_ctx {
    uint8_t                  mac[6];
    wei_conn_metric_record_t sample;
    wei_track_entry_t        snr_track;
    wei_track_entry_t        loss_track;
    wei_track_entry_t        phy_track;
    wei_track_entry_t        chanutil_track;
};

/* Shipped excursion policy: a per-metric degradation bound plus a sustained
 * dwell floor, applied when wei_life_init runs without an explicit policy and
 * overridable through that argument. These are tunable policy inputs handed to
 * the C3 tracker, never detector-baked constants. The dwell floor requires an
 * excursion to persist across three interval ticks before it counts. */
#define WEI_LIFE_DEFAULT_SNR_FLOOR_DB      15
#define WEI_LIFE_DEFAULT_LOSS_CEILING      10
#define WEI_LIFE_DEFAULT_PHY_FLOOR_KBPS    6000
#define WEI_LIFE_DEFAULT_CHANUTIL_CEIL_PCT 70
#define WEI_LIFE_DEFAULT_DWELL_MS          (3 * WEI_TICK_DEFAULT_MS)

static void wei_life_policy_set_defaults(wei_life_policy_t *policy)
{
    policy->snr_floor         = WEI_LIFE_DEFAULT_SNR_FLOOR_DB;
    policy->snr_dwell_ms      = WEI_LIFE_DEFAULT_DWELL_MS;
    policy->loss_ceiling      = WEI_LIFE_DEFAULT_LOSS_CEILING;
    policy->loss_dwell_ms     = WEI_LIFE_DEFAULT_DWELL_MS;
    policy->phy_floor         = WEI_LIFE_DEFAULT_PHY_FLOOR_KBPS;
    policy->phy_dwell_ms      = WEI_LIFE_DEFAULT_DWELL_MS;
    policy->chanutil_ceiling  = WEI_LIFE_DEFAULT_CHANUTIL_CEIL_PCT;
    policy->chanutil_dwell_ms = WEI_LIFE_DEFAULT_DWELL_MS;
}

/* The app-framework/HAL event surface delivers one decoded station-lifecycle
 * event per callback and carries no per-app cookie, so the unit binds its
 * roster to a single handler slot at init and reads it back on delivery. The
 * core wiring drives this slot from the existing assoc/disassoc/connection-change
 * callback; the unit reads from that surface only and forks neither the HAL nor
 * the core event queue. */
typedef void (*wei_life_station_sink_fn)(void *user, wei_life_event_t event,
    const uint8_t mac[6]);

static struct {
    wei_life_station_sink_fn deliver;
    void                    *user;
} g_wei_life_sink;

static void wei_life_station_sink(void *user, wei_life_event_t event, const uint8_t mac[6])
{
    wei_life_on_station_event((wei_life_roster_t *)user, event, mac);
}

/* Downstream excursion sink, wired by the integrator to the scorer-aggregation /
 * publish stage. Unset by default: the unit owns no downstream stage. */
static struct {
    wei_life_report_fn deliver;
    void              *user;
} g_wei_life_report;

void wei_life_set_report_sink(wei_life_report_fn sink, void *user)
{
    g_wei_life_report.deliver = sink;
    g_wei_life_report.user = user;
}

static void wei_life_report(const wei_life_excursion_t *excursion, uint8_t connected_score)
{
    if (g_wei_life_report.deliver != NULL) {
        g_wei_life_report.deliver(g_wei_life_report.user, excursion, connected_score);
    }
}

int wei_life_init(wei_life_roster_t *roster, const wei_life_policy_t *policy)
{
    if (roster == NULL) {
        return -1;
    }

    memset(roster, 0, sizeof(*roster));
    if (policy != NULL) {
        roster->policy = *policy;
    } else {
        wei_life_policy_set_defaults(&roster->policy);
    }

    if (pthread_mutex_init(&roster->guard, NULL) != 0) {
        return -1;
    }

    g_wei_life_sink.deliver = wei_life_station_sink;
    g_wei_life_sink.user = roster;
    return 0;
}

void wei_life_deinit(wei_life_roster_t *roster)
{
    if (roster == NULL) {
        return;
    }

    g_wei_life_sink.deliver = NULL;
    g_wei_life_sink.user = NULL;
    pthread_mutex_destroy(&roster->guard);
}

/* Index of the slot holding mac, or -1 when the station is untracked. Callers
 * hold roster->guard; the walk is bounded by the fixed roster capacity. */
static int wei_life_slot_of(const wei_life_roster_t *roster, const uint8_t mac[6])
{
    unsigned int i;

    for (i = 0; i < WEI_LIFE_MAX_STATIONS; i++) {
        if (roster->slots[i] != NULL &&
            memcmp(roster->slots[i]->mac, mac, sizeof(roster->slots[i]->mac)) == 0) {
            return (int)i;
        }
    }
    return -1;
}

wei_life_ctx_t *wei_life_attach_station(wei_life_roster_t *roster, const uint8_t mac[6])
{
    wei_life_ctx_t *ctx = NULL;
    unsigned int i;
    int idx;

    if (roster == NULL || mac == NULL) {
        return NULL;
    }

    pthread_mutex_lock(&roster->guard);

    idx = wei_life_slot_of(roster, mac);
    if (idx >= 0) {
        ctx = roster->slots[idx];   /* already attached: exactly one context */
        goto out;
    }

    for (i = 0; i < WEI_LIFE_MAX_STATIONS; i++) {
        if (roster->slots[i] == NULL) {
            ctx = (wei_life_ctx_t *)calloc(1, sizeof(*ctx));
            if (ctx != NULL) {
                memcpy(ctx->mac, mac, sizeof(ctx->mac));
                roster->slots[i] = ctx;
                roster->active++;
            }
            break;
        }
    }

out:
    pthread_mutex_unlock(&roster->guard);
    return ctx;
}

void wei_life_detach_station(wei_life_roster_t *roster, const uint8_t mac[6])
{
    int idx;

    if (roster == NULL || mac == NULL) {
        return;
    }

    pthread_mutex_lock(&roster->guard);

    idx = wei_life_slot_of(roster, mac);
    if (idx >= 0) {
        free(roster->slots[idx]);
        roster->slots[idx] = NULL;
        roster->active--;
    }

    pthread_mutex_unlock(&roster->guard);
}

void wei_life_ingest_metric(wei_life_roster_t *roster, const uint8_t mac[6],
    const wei_conn_metric_record_t *record)
{
    int idx;

    if (roster == NULL || mac == NULL || record == NULL) {
        return;
    }

    pthread_mutex_lock(&roster->guard);

    idx = wei_life_slot_of(roster, mac);
    if (idx >= 0) {
        roster->slots[idx]->sample = *record;
    }

    pthread_mutex_unlock(&roster->guard);
}

/* Connection-change for a station still on the air: keep its single context in
 * place so the interval detectors re-read the new link metrics on the next tick,
 * opening no duplicate. A station not already in the roster is left untouched --
 * only the assoc path opens a context. The lock spans the bounded lookup only. */
static void wei_life_reeval_station(wei_life_roster_t *roster, const uint8_t mac[6])
{
    pthread_mutex_lock(&roster->guard);
    if (wei_life_slot_of(roster, mac) < 0) {
        pthread_mutex_unlock(&roster->guard);
        return;
    }
    pthread_mutex_unlock(&roster->guard);
}

void wei_life_on_station_event(wei_life_roster_t *roster, wei_life_event_t event,
    const uint8_t mac[6])
{
    if (roster == NULL || mac == NULL) {
        return;
    }

    switch (event) {
    case WEI_LIFE_EVENT_ASSOC:
        wei_life_attach_station(roster, mac);
        break;
    case WEI_LIFE_EVENT_DISASSOC:
        wei_life_detach_station(roster, mac);
        break;
    case WEI_LIFE_EVENT_CONN_CHANGE:
        wei_life_reeval_station(roster, mac);
        break;
    default:
        break;
    }
}

/* LQ-2 SNR-degradation detector. Reads the client's normalized SNR from the C1
 * record and delegates all crossing-with-duration timing to the C3 tracker
 * against the configurable degradation floor; it owns no excursion timing and
 * deliberately reaches no steering RSSI-crossing path, whose semantics differ.
 * *out is filled and 1 returned only for an interval carrying an onset or a
 * finalized dwell. */
static int wei_life_eval_snr(wei_life_ctx_t *ctx, const wei_conn_metric_record_t *m,
    const wei_life_policy_t *policy, int64_t now_ms, wei_life_excursion_t *out)
{
    wei_track_record_t rec;

    wei_track_tick(&ctx->snr_track, m->link_snr_db, now_ms,
        policy->snr_floor, policy->snr_dwell_ms);
    wei_track_collect(&ctx->snr_track, &rec);
    if (rec.count == 0 && rec.duration_ms == 0) {
        return 0;
    }

    memcpy(out->mac, ctx->mac, sizeof(out->mac));
    out->metric = WEI_LIFE_METRIC_SNR;
    out->onset_count = rec.count;
    out->duration_ms = rec.duration_ms;
    return 1;
}

/* LAT-4 packet-loss detector. Reads the client's normalized PER from the C1
 * record and delegates all crossing-with-duration timing to the C3 tracker
 * against the configurable loss ceiling; it owns no excursion timing and touches
 * no assoc-devices collector or measurement sink. The tracker opens an excursion
 * below its band, so the above-ceiling loss condition is expressed by negating
 * both the value and the ceiling. *out is filled and 1 returned only for an
 * interval carrying an onset or a finalized dwell. */
static int wei_life_eval_loss(wei_life_ctx_t *ctx, const wei_conn_metric_record_t *m,
    const wei_life_policy_t *policy, int64_t now_ms, wei_life_excursion_t *out)
{
    wei_track_record_t rec;

    wei_track_tick(&ctx->loss_track, -(int32_t)m->pkt_err_rate, now_ms,
        -policy->loss_ceiling, policy->loss_dwell_ms);
    wei_track_collect(&ctx->loss_track, &rec);
    if (rec.count == 0 && rec.duration_ms == 0) {
        return 0;
    }

    memcpy(out->mac, ctx->mac, sizeof(out->mac));
    out->metric = WEI_LIFE_METRIC_LOSS;
    out->onset_count = rec.count;
    out->duration_ms = rec.duration_ms;
    return 1;
}

/* TP-1 PHY-rate-shortfall detector. Reads the client's normalized PHY rate from
 * the C1 record and delegates all crossing-with-duration timing to the C3 tracker
 * against the fixed configurable PHY floor; it owns no excursion timing. The v1
 * floor is a plain policy input -- no negotiated-capability (FE-4) lookup is
 * introduced here. Realized PHY rates sit far below INT32_MAX, so the tracker's
 * signed band comparison holds. *out is filled and 1 returned only for an
 * interval carrying an onset or a finalized dwell. */
static int wei_life_eval_phy(wei_life_ctx_t *ctx, const wei_conn_metric_record_t *m,
    const wei_life_policy_t *policy, int64_t now_ms, wei_life_excursion_t *out)
{
    wei_track_record_t rec;

    wei_track_tick(&ctx->phy_track, (int32_t)m->phy_rate_kbps, now_ms,
        policy->phy_floor, policy->phy_dwell_ms);
    wei_track_collect(&ctx->phy_track, &rec);
    if (rec.count == 0 && rec.duration_ms == 0) {
        return 0;
    }

    memcpy(out->mac, ctx->mac, sizeof(out->mac));
    out->metric = WEI_LIFE_METRIC_PHY;
    out->onset_count = rec.count;
    out->duration_ms = rec.duration_ms;
    return 1;
}

/* TP-2 channel-utilization-impact detector. Reads the client's normalized
 * channel utilization from the C1 record and delegates all crossing-with-duration
 * timing to the C3 tracker against the configurable impact ceiling; it owns no
 * excursion timing. The excursion routes to the connected scorer path only and
 * deliberately reaches no radio channel-utilization telemetry upload seam, whose
 * semantics differ. The tracker opens an excursion below its band, so the
 * above-ceiling impact condition is expressed by negating both the value and the
 * ceiling. *out is filled and 1 returned only for an interval carrying an onset
 * or a finalized dwell. */
static int wei_life_eval_chanutil(wei_life_ctx_t *ctx, const wei_conn_metric_record_t *m,
    const wei_life_policy_t *policy, int64_t now_ms, wei_life_excursion_t *out)
{
    wei_track_record_t rec;

    wei_track_tick(&ctx->chanutil_track, -(int32_t)m->chan_util_pct, now_ms,
        -policy->chanutil_ceiling, policy->chanutil_dwell_ms);
    wei_track_collect(&ctx->chanutil_track, &rec);
    if (rec.count == 0 && rec.duration_ms == 0) {
        return 0;
    }

    memcpy(out->mac, ctx->mac, sizeof(out->mac));
    out->metric = WEI_LIFE_METRIC_CHANUTIL;
    out->onset_count = rec.count;
    out->duration_ms = rec.duration_ms;
    return 1;
}

static int64_t wei_life_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Drives the four per-metric detectors over one context's freshest C1 sample,
 * packing each surfaced excursion into out[] (capacity WEI_LIFE_METRIC_COUNT) and
 * returning how many were produced this interval. Every detector delegates its
 * crossing-with-duration timing to the C3 tracker; this unit reimplements none. */
static unsigned int wei_life_eval_ctx(wei_life_ctx_t *ctx, const wei_life_policy_t *policy,
    int64_t now_ms, wei_life_excursion_t *out)
{
    const wei_conn_metric_record_t *m = &ctx->sample;
    unsigned int n = 0;

    n += (unsigned int)wei_life_eval_snr(ctx, m, policy, now_ms, &out[n]);
    n += (unsigned int)wei_life_eval_loss(ctx, m, policy, now_ms, &out[n]);
    n += (unsigned int)wei_life_eval_phy(ctx, m, policy, now_ms, &out[n]);
    n += (unsigned int)wei_life_eval_chanutil(ctx, m, policy, now_ms, &out[n]);
    return n;
}

/* C3 interval-tick callback: walks the roster once and evaluates every active
 * context. The guard is taken per slot to cover the set-read and the detectors'
 * bounded, non-blocking context mutation, then dropped before the heavier scorer
 * reduction and downstream delivery run on a private snapshot -- the lock never
 * spans the scorer path. The unit owns no timer or poller thread. */
void wei_life_eval_station(void *user)
{
    wei_life_roster_t *roster = (wei_life_roster_t *)user;
    int64_t now_ms;
    unsigned int i;

    if (roster == NULL) {
        return;
    }

    now_ms = wei_life_now_ms();

    for (i = 0; i < WEI_LIFE_MAX_STATIONS; i++) {
        wei_life_excursion_t     batch[WEI_LIFE_METRIC_COUNT];
        wei_conn_metric_record_t sample;
        wei_life_ctx_t          *ctx;
        unsigned int             produced = 0;
        unsigned int             k;
        uint8_t                  score;

        pthread_mutex_lock(&roster->guard);
        ctx = roster->slots[i];
        if (ctx != NULL) {
            produced = wei_life_eval_ctx(ctx, &roster->policy, now_ms, batch);
            sample = ctx->sample;
        }
        pthread_mutex_unlock(&roster->guard);

        if (produced == 0) {
            continue;
        }

        score = wei_conn_scorer_score(&sample);
        for (k = 0; k < produced; k++) {
            wei_life_report(&batch[k], score);
        }
    }
}

wei_poll_on_tick wei_life_tick_fn(void)
{
    return wei_life_eval_station;
}
