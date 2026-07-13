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

#ifndef WEI_INFER_H
#define WEI_INFER_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Consumed contracts, bound by their exact declared names: the C1 per-client
 * metric record this engine scores read-only, the C5 stateless kernel it
 * delegates every normalise/reduce/weight/standardise step to, and the C3
 * interval cadence and crossing-with-duration tracker it drives. This engine
 * owns per-client state around these; it redefines none of them. */
#include "wei_conn_metric.h"
#include "wei_conn_scorer.h"
#include "wei_poll.h"
#include "wei_track.h"

/* Per-client serviceable-state verdict evaluated each tick: the smooth-to-
 * degraded toggle carried to the caller alongside the score. */
typedef enum {
    WEI_INFER_VERDICT_SMOOTH   = 0,
    WEI_INFER_VERDICT_DEGRADED = 1
} wei_infer_verdict_t;

/* Metric attributed as the dominant contributor to a degradation verdict
 * (TP-4). NONE when attribution is deferred, which never alters the score. */
typedef enum {
    WEI_INFER_CONTRIB_NONE      = 0,
    WEI_INFER_CONTRIB_SNR,
    WEI_INFER_CONTRIB_PHY,
    WEI_INFER_CONTRIB_PKT_ERR,
    WEI_INFER_CONTRIB_CHAN_UTIL
} wei_infer_contributor_t;

/* Caller-supplied thresholds and floors the engine must never bake in: the
 * throughput-instability band and sustained-duration floor handed to the C3
 * tracker (TP-3), the low-score band and consecutive-tick count bounding a
 * latency-risk period (LAT-2), and the score band that labels a verdict
 * video-degraded (BUF-1). */
typedef struct {
    int32_t  tput_var_band;          /* TP-3: instability band -> wei_track_tick */
    int64_t  tput_floor_ms;          /* TP-3: sustained-vs-brief floor -> wei_track_tick */
    uint8_t  latency_score_band;     /* LAT-2: score below this counts toward an at-risk period */
    uint16_t latency_sustain_ticks;  /* LAT-2: consecutive sub-band ticks marking the period */
    uint8_t  video_degrade_band;     /* BUF-1: score band that labels a verdict video-degraded */
} wei_infer_policy_t;

/* One per-client inference result emitted per tick: the standardised 0-100
 * connected-P score, its serviceable-state verdict, the reserved TP-4 dominant-
 * contributor tag, and the reserved BUF-1 video-degrade label. Value-only
 * output; all RBus/accessor/publish egress is owned downstream. */
typedef struct {
    uint8_t                 score;
    wei_infer_verdict_t     verdict;
    wei_infer_contributor_t dominant;
    uint8_t                 video_degrade;
} wei_infer_result_t;

/* Normalised score metrics carrying a running per-client band, in slot order:
 * link SNR, uplink PHY rate, packet-error rate. Channel utilisation is a
 * weighting input to the C5 kernel rather than a banded metric, so it is not
 * counted here. */
#define WEI_INFER_METRIC_COUNT 3

/* Depth of the packet-error sliding window and the smoothed uplink-PHY history
 * window kept per client for the throughput-instability measure (TP-3). */
#define WEI_INFER_PER_WINDOW 8
#define WEI_INFER_PHY_WINDOW 8

/* Running observed [lo,hi] domain for one normalised metric, widened per client
 * so cscore_normalize_metric maps against this client's own dynamic band rather
 * than a fixed one; primed is 0 until the first sample seeds lo == hi. */
typedef struct {
    double  lo;
    double  hi;
    uint8_t primed;
} wei_infer_band_t;

/* Fixed-size per-client inference state: the running normalisation bands, the
 * packet-error and smoothed uplink-PHY sliding windows (ring-indexed by head/
 * fill), and the connection-lifecycle counters (threshold-cross run, latched
 * alarm, disconnect tracking, rapid-reconnect recovery ramp). One block per
 * scored client; sizeof is a compile-time constant and the sampling path reads
 * and updates it with no allocation. Transposed from the reference scoring-state
 * shape only -- fresh names, no reference identifier reused. */
typedef struct {
    wei_infer_band_t norm_band[WEI_INFER_METRIC_COUNT];

    uint16_t per_window[WEI_INFER_PER_WINDOW];
    uint8_t  per_head;
    uint8_t  per_fill;

    uint32_t phy_window[WEI_INFER_PHY_WINDOW];
    uint8_t  phy_head;
    uint8_t  phy_fill;

    uint16_t threshold_cross;
    uint8_t  alarm;
    uint8_t  disconnected;
    int64_t  disconnect_ts;
    uint16_t reconnect_ramp;
} wei_infer_client_t;

/* One membership slot: a station the engine scores, keyed by its MAC and owning
 * that client's inference state. in_use marks the slot live and last_seen stamps
 * its most recent tick so the sweep can reap departed stations. */
typedef struct {
    uint8_t            mac[6];
    uint8_t            in_use;
    int64_t            last_seen;
    wei_infer_client_t state;
} wei_infer_slot_t;

/* Bounded per-client membership map: the fixed-capacity per-station set the sweep
 * scores each tick, ceilinged at the consumed WEI_TRACK_MAX_CLIENTS with no per-
 * entry heap. Self-owned and reconciled/reaped on the C3 tick, coupled to no C2
 * lifecycle-roster symbol so the pillars stay decoupled. Transposed from the
 * reference station-map shape only; no reference identifier reused. */
typedef struct {
    wei_infer_slot_t slots[WEI_TRACK_MAX_CLIENTS];
    uint8_t          count;
} wei_infer_map_t;

/* Capacity of the per-client verdict-episode ring: the most recent degradation
 * episodes retained for the recurrence measure (BUF-2, CH-2). */
#define WEI_INFER_EPISODE_CAP 8

/* One serviceable-state degradation episode: the interval a client's verdict held
 * degraded, opened at start_ms and closed at stop_ms on recovery. stop_ms stays 0
 * while the episode is still open. */
typedef struct {
    int64_t start_ms;
    int64_t stop_ms;
} wei_infer_episode_t;

/* Fixed-size per-client verdict-episode ledger behind the smooth-to-degraded
 * toggle: episodes counts every degradation the client has entered (the recurring-
 * adverse-outcome frequency, saturating), while ring keeps the most recent
 * WEI_INFER_EPISODE_CAP of them with their start/stop timestamps (the per-episode
 * duration), head marking the current episode slot and fill the live entries. A
 * fresh C4 structure distinct from the consumed per-metric excursion tracker,
 * which it neither redefines nor forks. Transposed from the reference serviceable-
 * state toggle and alarm/time history shape only; no reference identifier reused. */
typedef struct {
    wei_infer_episode_t ring[WEI_INFER_EPISODE_CAP];
    uint8_t  head;
    uint8_t  fill;
    uint32_t episodes;
} wei_infer_episode_log_t;

/* Opaque scorer-engine instance. Created without owning a receiver: the daemon
 * owns the single poll loop and feeds this engine through the entries below, so
 * exactly one context owns the AF_UNIX socket. */
typedef struct wei_infer_engine wei_infer_engine_t;

/* Allocates an engine with the caller's fixed policy (the engine bakes none of
 * it). It owns its per-client membership map and excursion tracker but no socket
 * or poll loop. Returns NULL on a NULL policy or allocation failure. */
wei_infer_engine_t *wei_infer_create(const wei_infer_policy_t *policy);

/* Stages one client's latest metric record for the next tick, admitting the MAC
 * into the bounded membership map on first sight and refreshing its last-seen
 * stamp; a full map drops the record. Runs on the daemon poll thread, takes no
 * lock, and allocates nothing. */
void wei_infer_stage_record(wei_infer_engine_t *eng, const uint8_t mac[6],
    const wei_conn_metric_record_t *rec);

/* Scores every live client for this interval and publishes each result; the
 * daemon invokes it once per poll tick after that interval's records are staged.
 * NULL-safe. */
void wei_infer_tick(wei_infer_engine_t *eng);

/* Releases the engine. NULL-safe. */
void wei_infer_destroy(wei_infer_engine_t *eng);

#ifdef __cplusplus
}
#endif
#endif /* WEI_INFER_H */
