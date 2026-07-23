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

#include "wei_infer.h"
#include "wei_perfreport.h"
#include "wei_util.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* One scorer-engine instance: the self-owned per-client membership map scored
 * each interval, the consumed C3 crossing-with-duration tracker driven for
 * per-metric excursion timing, the caller-supplied policy thresholds/floors
 * (never baked here), and the per-client input/output buffers the sweep works
 * over: snapshot[i] is the latest C1 record staged for map slot i (read-only
 * while scoring), result[i] its latest score+verdict, and ledger[i] its
 * serviceable-state verdict-episode recurrence history -- all retained for
 * downstream pull. The daemon owns the single poll loop and feeds this engine
 * per record and per tick, so the engine lives entirely on that one poll thread,
 * owns no lock, and reaches into none of the core data-plane locks. */
struct wei_infer_engine {
    wei_infer_map_t          map;
    wei_track_table_t        tracker;
    wei_infer_policy_t       policy;
    wei_conn_metric_record_t snapshot[WEI_TRACK_MAX_CLIENTS];
    wei_infer_result_t       result[WEI_TRACK_MAX_CLIENTS];
    wei_infer_episode_log_t  ledger[WEI_TRACK_MAX_CLIENTS];
};

/* Ticks over which a freshly (re)admitted client's score is damped back to full
 * trust as its windows refill -- an internal recovery shape, not a caller policy
 * input, so it stays out of wei_infer_policy_t. */
#define WEI_INFER_RECONNECT_RAMP 4u

static int64_t wei_infer_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Releases membership slots for stations unseen past the consumed absence
 * window, returning each fixed slot and its per-client verdict-episode ledger to
 * the WEI_TRACK_MAX_CLIENTS pool so a reused slot starts with no inherited
 * recurrence history. O(map), no allocation. */
static void wei_infer_map_reap(wei_infer_engine_t *eng, int64_t now_ms)
{
    unsigned int i;

    for (i = 0; i < WEI_TRACK_MAX_CLIENTS; i++) {
        wei_infer_slot_t *slot = &eng->map.slots[i];

        if (slot->in_use && now_ms - slot->last_seen > WEI_TRACK_ABSENCE_DEFAULT_MS) {
            wei_util_info_print(WEI_CONNECTED,
                "%s:%d [REAP] mac=%02x:%02x:%02x:%02x:%02x:%02x absent %lldms -> slot %u freed\n",
                __func__, __LINE__, slot->mac[0], slot->mac[1], slot->mac[2],
                slot->mac[3], slot->mac[4], slot->mac[5],
                (long long)(now_ms - slot->last_seen), i);
            memset(slot, 0, sizeof(*slot));
            memset(&eng->ledger[i], 0, sizeof(eng->ledger[i]));
            if (eng->map.count > 0) {
                eng->map.count--;
            }
        }
    }
}

/* Pushes this tick's raw packet-error and uplink-PHY samples into their ring
 * windows, advancing head and saturating fill; O(1), no allocation. */
static void wei_infer_per_push(wei_infer_client_t *st, uint16_t v)
{
    st->per_window[st->per_head] = v;
    st->per_head = (uint8_t)((st->per_head + 1u) % WEI_INFER_PER_WINDOW);
    if (st->per_fill < WEI_INFER_PER_WINDOW) {
        st->per_fill++;
    }
}

static void wei_infer_phy_push(wei_infer_client_t *st, uint32_t v)
{
    st->phy_window[st->phy_head] = v;
    st->phy_head = (uint8_t)((st->phy_head + 1u) % WEI_INFER_PHY_WINDOW);
    if (st->phy_fill < WEI_INFER_PHY_WINDOW) {
        st->phy_fill++;
    }
}

static double wei_infer_per_mean(const wei_infer_client_t *st)
{
    uint32_t sum = 0;
    uint8_t i;

    for (i = 0; i < st->per_fill; i++) {
        sum += st->per_window[i];
    }
    return st->per_fill ? (double)sum / (double)st->per_fill : 0.0;
}

static double wei_infer_phy_mean(const wei_infer_client_t *st)
{
    uint64_t sum = 0;
    uint8_t i;

    for (i = 0; i < st->phy_fill; i++) {
        sum += st->phy_window[i];
    }
    return st->phy_fill ? (double)sum / (double)st->phy_fill : 0.0;
}

/* Throughput instability of the smoothed uplink-PHY window: its population
 * variance over the D1 samples, scaled to (Mbps)^2 so a full-rate swing stays
 * within int32 and the policy band's units. Engine-side D1 math only -- no C5
 * reduce/normalise step. Under two samples there is no spread to measure, so it
 * reads as stable. (TP-3) */
static int32_t wei_infer_phy_instability(const wei_infer_client_t *st)
{
    double mean;
    double accum = 0.0;
    uint8_t i;

    if (st->phy_fill < 2) {
        return 0;
    }

    mean = wei_infer_phy_mean(st);
    for (i = 0; i < st->phy_fill; i++) {
        double d = (double)st->phy_window[i] - mean;

        accum += d * d;
    }
    {
        int32_t instab = (int32_t)(accum / (double)st->phy_fill / 1.0e6);
        wei_util_dbg_print(WEI_CONNECTED,
            "%s:%d [TP-3 instability] phy_fill=%u mean=%.0f var=%.0f -> instab=%d (Mbps^2)\n",
            __func__, __LINE__, (unsigned)st->phy_fill, mean,
            accum / (double)st->phy_fill, instab);
        return instab;
    }
}

/* Widens the client's running observed [lo,hi] domain to include v, seeding it on
 * the first sample so cscore_normalize_metric maps against this client's own
 * dynamic band rather than a fixed one. */
static void wei_infer_band_widen(wei_infer_band_t *b, double v)
{
    if (!b->primed) {
        b->lo = v;
        b->hi = v;
        b->primed = 1;
        wei_util_dbg_print(WEI_CONNECTED, "%s:%d [BAND] prime v=%.2f -> [%.2f,%.2f]\n",
            __func__, __LINE__, v, b->lo, b->hi);
        return;
    }
    if (v < b->lo) {
        b->lo = v;
    }
    if (v > b->hi) {
        b->hi = v;
    }
    wei_util_dbg_print(WEI_CONNECTED, "%s:%d [BAND] v=%.2f -> [%.2f,%.2f]\n",
        __func__, __LINE__, v, b->lo, b->hi);
}

/* Scores one client for this tick: gates on the read-only C1 activity/status,
 * smooths the tick's raw sample into the D1 windows, widens the client's own
 * running normalisation bands, and drives the committed C5 kernel helpers over
 * those bands -- normalise (PER enters negative so the signed-square RMS
 * penalises it) -> RMS reduce -> channel-utilisation weight -> standardise. The
 * standardised 0-100 score is damped while a freshly (re)admitted client's
 * rapid-reconnect ramp recovers. Returns 1 when a result is emitted into *out, 0
 * when the client is gated out this tick. The snapshot is never mutated and no
 * allocation occurs; the smooth/reduce/weight/standardise math stays in C5. */
static int wei_infer_score_client(wei_infer_client_t *st,
    const wei_conn_metric_record_t *rec, wei_infer_result_t *out)
{
    double norm[WEI_INFER_METRIC_COUNT];
    double smoothed_phy;
    double smoothed_per;
    double reduced;
    double weighted;
    double score;
    double raw_score;
    int warming;

    if (rec->activity_state != WEI_CONN_METRIC_STATE_ACTIVE ||
        rec->status != WEI_CONN_METRIC_STATUS_OK) {
        wei_util_dbg_print(WEI_CONNECTED,
            "%s:%d [SCORE-CLIENT] gated: active=%d status=%d -> no score this tick\n",
            __func__, __LINE__, (int)rec->activity_state, (int)rec->status);
        return 0;
    }

    warming = (st->phy_fill == 0);

    wei_infer_phy_push(st, rec->phy_rate_kbps);
    wei_infer_per_push(st, rec->pkt_err_rate);
    smoothed_phy = wei_infer_phy_mean(st);
    smoothed_per = wei_infer_per_mean(st);

    wei_infer_band_widen(&st->norm_band[0], (double)rec->link_snr_db);
    wei_infer_band_widen(&st->norm_band[1], smoothed_phy);
    wei_infer_band_widen(&st->norm_band[2], smoothed_per);

    norm[0] =  cscore_normalize_metric((double)rec->link_snr_db,
                                       st->norm_band[0].lo, st->norm_band[0].hi);
    norm[1] =  cscore_normalize_metric(smoothed_phy,
                                       st->norm_band[1].lo, st->norm_band[1].hi);
    norm[2] = -cscore_normalize_metric(smoothed_per,
                                       st->norm_band[2].lo, st->norm_band[2].hi);

    reduced  = cscore_rms_reduce(norm, WEI_INFER_METRIC_COUNT);
    weighted = cscore_chanutil_weight(reduced, (double)rec->chan_util_pct / 100.0);
    score    = cscore_standardize(weighted);
    raw_score = score;

    if (warming) {
        st->reconnect_ramp = WEI_INFER_RECONNECT_RAMP;
    }
    if (st->reconnect_ramp > 0) {
        score = score * (double)(WEI_INFER_RECONNECT_RAMP - st->reconnect_ramp + 1u) /
                (double)WEI_INFER_RECONNECT_RAMP;
        st->reconnect_ramp--;
    }

    out->score = (uint8_t)(score + 0.5);
    out->verdict = WEI_INFER_VERDICT_SMOOTH;
    out->dominant = WEI_INFER_CONTRIB_NONE;
    out->video_degrade = 0;

    wei_util_info_print(WEI_CONNECTED,
        "%s:%d [SCORE-CLIENT] in{snr=%d phy_kbps=%u per=%u chan=%u} smoothed{phy=%.0f per=%.2f} "
        "bands{snr[%.1f,%.1f] phy[%.0f,%.0f] per[%.2f,%.2f]} norm{%.3f %.3f %.3f} "
        "reduced=%.4f weighted=%.4f raw=%.2f ramp_left=%u -> score=%u\n",
        __func__, __LINE__, rec->link_snr_db, rec->phy_rate_kbps,
        (unsigned)rec->pkt_err_rate, (unsigned)rec->chan_util_pct,
        smoothed_phy, smoothed_per,
        st->norm_band[0].lo, st->norm_band[0].hi, st->norm_band[1].lo, st->norm_band[1].hi,
        st->norm_band[2].lo, st->norm_band[2].hi, norm[0], norm[1], norm[2],
        reduced, weighted, raw_score, (unsigned)st->reconnect_ramp, (unsigned)out->score);
    return 1;
}

/* Times and flags this client's throughput-instability contribution for the tick
 * (TP-3). The variance of its smoothed uplink-PHY window drives the consumed C3
 * crossing-with-duration tracker; because that tracker opens an excursion when its
 * metric drops below band, the instability and the policy band are both negated so
 * a variance spike past wei_infer_policy_t::tput_var_band opens a duration-timed
 * excursion, with brief blips floored out by tput_floor_ms -- neither value baked
 * here. A current spike or a just-closed sustained excursion marks the verdict
 * degraded; the 0-100 score stays C5-owned and the C1 record read-only. */
static void wei_infer_flag_instability(wei_infer_engine_t *eng, wei_infer_slot_t *slot,
    wei_infer_result_t *out, int64_t now_ms)
{
    wei_track_record_t excursion;
    wei_track_entry_t *entry;
    int32_t instability;
    int unstable;
    int64_t exc_ms = 0;

    instability = wei_infer_phy_instability(&slot->state);
    unstable = instability > eng->policy.tput_var_band;

    entry = wei_track_get(&eng->tracker, slot->mac, now_ms);
    if (entry != NULL) {
        wei_track_tick(entry, -instability, now_ms, -eng->policy.tput_var_band,
                       eng->policy.tput_floor_ms);
        wei_track_collect(entry, &excursion);
        exc_ms = excursion.duration_ms;
        unstable = unstable || exc_ms > 0;
    }

    if (unstable) {
        out->verdict = WEI_INFER_VERDICT_DEGRADED;
    }

    wei_util_dbg_print(WEI_CONNECTED,
        "%s:%d [FLAG-instability] instab=%d band=%d excursion_ms=%lld -> %s\n",
        __func__, __LINE__, instability, eng->policy.tput_var_band, (long long)exc_ms,
        unstable ? "DEGRADED" : "ok");
}

/* Marks a latency-driven negative-experience period over the connected-P score
 * band (LAT-2). No on-box latency source exists, so a sustained low score stands
 * in: each tick the standardised score sits below wei_infer_policy_t::
 * latency_score_band advances the client's sub-band run and, once it holds for
 * latency_sustain_ticks, latches the alarm and drives the verdict degraded through
 * the same D4 episode machinery a throughput excursion uses; any at-band tick
 * clears the run and the alarm. Both the band and the tick count are policy inputs,
 * never baked, and no latency or jitter field is read or stored. */
static void wei_infer_flag_latency_risk(wei_infer_engine_t *eng, wei_infer_client_t *st,
    wei_infer_result_t *out)
{
    if (out->score < eng->policy.latency_score_band) {
        if (st->threshold_cross < UINT16_MAX) {
            st->threshold_cross++;
        }
        if (st->threshold_cross >= eng->policy.latency_sustain_ticks) {
            st->alarm = 1;
        }
    } else {
        st->threshold_cross = 0;
        st->alarm = 0;
    }

    if (st->alarm) {
        out->verdict = WEI_INFER_VERDICT_DEGRADED;
    }

    wei_util_dbg_print(WEI_CONNECTED,
        "%s:%d [FLAG-latency] score=%u band=%u cross=%u/%u alarm=%u -> %s\n",
        __func__, __LINE__, (unsigned)out->score, (unsigned)eng->policy.latency_score_band,
        (unsigned)st->threshold_cross, (unsigned)eng->policy.latency_sustain_ticks,
        (unsigned)st->alarm, st->alarm ? "DEGRADED" : "ok");
}

/* Labels the client's result video-degraded when its connected-P score sits in the
 * buffering-correlated band (BUF-1). A standardised score below wei_infer_policy_t::
 * video_degrade_band -- a policy input, never baked -- marks network conditions
 * correlated with streaming degradation; the label is additive metadata on the single
 * P score, read only from the already-scored network metrics. No app-layer buffering
 * signal is read and no separate video classifier exists, and the score and verdict
 * are left unchanged. */
static void wei_infer_flag_video_degrade(const wei_infer_policy_t *policy,
    wei_infer_result_t *out)
{
    if (out->score < policy->video_degrade_band) {
        out->video_degrade = 1;
    }

    wei_util_dbg_print(WEI_CONNECTED,
        "%s:%d [FLAG-video] score=%u band=%u -> video_degrade=%u\n",
        __func__, __LINE__, (unsigned)out->score, (unsigned)policy->video_degrade_band,
        (unsigned)out->video_degrade);
}

/* Attributes the dominant metric behind a throughput-degradation verdict (TP-4,
 * SHOULD). Co-reads the client's simultaneous C1 record metrics for this tick and,
 * over the client's own running normalisation bands, ranks each by how far it sits
 * in its degrading direction -- low for the reward metrics (SNR, uplink PHY), high
 * for the penalty metrics (packet error, channel utilisation) -- and returns the
 * worst. Pure read-side selection over inputs C5 already consumes: the record is
 * never mutated, no metric is added, and the tag never affects the score. */
static wei_infer_contributor_t wei_infer_attribute(const wei_infer_client_t *st,
    const wei_conn_metric_record_t *rec)
{
    double badness[WEI_INFER_METRIC_COUNT + 1];
    unsigned int worst = 0;
    unsigned int i;

    badness[0] = 1.0 - cscore_normalize_metric((double)rec->link_snr_db,
                                               st->norm_band[0].lo, st->norm_band[0].hi);
    badness[1] = 1.0 - cscore_normalize_metric((double)rec->phy_rate_kbps,
                                               st->norm_band[1].lo, st->norm_band[1].hi);
    badness[2] = cscore_normalize_metric((double)rec->pkt_err_rate,
                                         st->norm_band[2].lo, st->norm_band[2].hi);
    badness[3] = (double)rec->chan_util_pct / 100.0;

    for (i = 1; i < WEI_INFER_METRIC_COUNT + 1; i++) {
        if (badness[i] > badness[worst]) {
            worst = i;
        }
    }

    wei_util_dbg_print(WEI_CONNECTED,
        "%s:%d [ATTRIBUTE] badness{snr=%.3f phy=%.3f per=%.3f chan=%.3f} -> dominant=%d\n",
        __func__, __LINE__, badness[0], badness[1], badness[2], badness[3],
        (int)(WEI_INFER_CONTRIB_SNR + worst));
    return (wei_infer_contributor_t)(WEI_INFER_CONTRIB_SNR + worst);
}

/* Folds the client's smooth<->degraded verdict recurrence into its D4 episode
 * ledger each tick (BUF-2, CH-2). Whether an episode is currently open is read
 * back from the ledger itself -- the most recent ring entry with no stop stamp --
 * so no duplicate recurrence state is kept. A smooth->degraded transition opens a
 * fresh episode: it stamps the start, advances the capped ring, and saturates the
 * bounded recurring-adverse-outcome frequency count; degraded->smooth closes the
 * open episode with its stop stamp. Strictly per-client -- one ledger per station,
 * no cross-client grouping; O(1), no allocation, and the consumed wei_track
 * tracker is left untouched. */
static void wei_infer_ledger_update(wei_infer_episode_log_t *log,
    wei_infer_verdict_t verdict, int64_t now_ms)
{
    int open = log->fill > 0 && log->ring[log->head].stop_ms == 0;

    if (verdict == WEI_INFER_VERDICT_DEGRADED) {
        if (!open) {
            if (log->fill > 0) {
                log->head = (uint8_t)((log->head + 1u) % WEI_INFER_EPISODE_CAP);
            } else {
                log->head = 0;
            }
            log->ring[log->head].start_ms = now_ms;
            log->ring[log->head].stop_ms = 0;
            if (log->fill < WEI_INFER_EPISODE_CAP) {
                log->fill++;
            }
            if (log->episodes < UINT32_MAX) {
                log->episodes++;
            }
        }
    } else if (open) {
        log->ring[log->head].stop_ms = now_ms;
    }

    wei_util_dbg_print(WEI_CONNECTED,
        "%s:%d [LEDGER] verdict=%s was_open=%d -> episodes=%u fill=%u\n",
        __func__, __LINE__,
        verdict == WEI_INFER_VERDICT_DEGRADED ? "degraded" : "smooth",
        open, log->episodes, (unsigned)log->fill);
}

/* Daemon per-tick entry, invoked once per poll interval after that interval's
 * records are staged. Runs on the single poll thread, so scoring and membership
 * maintenance take no lock. Sweeps the membership map, scoring each live client
 * into its result slot, flagging its throughput-instability contribution and
 * publishing its report, then reaps the excursion tracker and departed stations
 * past the consumed absence window. */
void wei_infer_tick(wei_infer_engine_t *eng)
{
    int64_t now_ms;
    unsigned int i;

    if (eng == NULL) {
        return;
    }

    now_ms = wei_infer_now_ms();

    wei_util_dbg_print(WEI_CONNECTED, "%s:%d [SWEEP] scoring %u client(s) in map\n",
        __func__, __LINE__, (unsigned)eng->map.count);

    for (i = 0; i < WEI_TRACK_MAX_CLIENTS; i++) {
        wei_infer_slot_t *slot = &eng->map.slots[i];

        if (!slot->in_use) {
            continue;
        }
        if (wei_infer_score_client(&slot->state, &eng->snapshot[i], &eng->result[i])) {
            wei_infer_flag_instability(eng, slot, &eng->result[i], now_ms);
            wei_infer_flag_latency_risk(eng, &slot->state, &eng->result[i]);
            wei_infer_flag_video_degrade(&eng->policy, &eng->result[i]);
            if (eng->result[i].verdict == WEI_INFER_VERDICT_DEGRADED) {
                eng->result[i].dominant =
                    wei_infer_attribute(&slot->state, &eng->snapshot[i]);
            }
            wei_infer_ledger_update(&eng->ledger[i], eng->result[i].verdict, now_ms);
            wei_util_info_print(WEI_CONNECTED,
                "%s:%d [SCORE] mac=%02x:%02x:%02x:%02x:%02x:%02x score=%u verdict=%s "
                "dominant=%d video=%u\n",
                __func__, __LINE__, slot->mac[0], slot->mac[1], slot->mac[2],
                slot->mac[3], slot->mac[4], slot->mac[5], (unsigned)eng->result[i].score,
                eng->result[i].verdict == WEI_INFER_VERDICT_DEGRADED ? "degraded" : "smooth",
                (int)eng->result[i].dominant, (unsigned)eng->result[i].video_degrade);
            wei_perfreport_publish_tick(slot->mac, &eng->result[i], &eng->snapshot[i]);
        }
    }

    wei_track_reap(&eng->tracker, now_ms, WEI_TRACK_ABSENCE_DEFAULT_MS);
    wei_infer_map_reap(eng, now_ms);
}

wei_infer_engine_t *wei_infer_create(const wei_infer_policy_t *policy)
{
    wei_infer_engine_t *eng;

    if (policy == NULL) {
        return NULL;
    }

    eng = (wei_infer_engine_t *)calloc(1, sizeof(*eng));
    if (eng == NULL) {
        return NULL;
    }

    wei_track_init(&eng->tracker);
    eng->policy = *policy;

    wei_util_info_print(WEI_CONNECTED,
        "%s:%d [ENGINE] created: policy{tput_var=%d tput_floor=%lldms lat_band=%u "
        "lat_sustain=%u video_band=%u} capacity=%d\n",
        __func__, __LINE__, eng->policy.tput_var_band, (long long)eng->policy.tput_floor_ms,
        (unsigned)eng->policy.latency_score_band, (unsigned)eng->policy.latency_sustain_ticks,
        (unsigned)eng->policy.video_degrade_band, WEI_TRACK_MAX_CLIENTS);
    return eng;
}

/* Admits the MAC into the bounded map on first sight and stages this interval's
 * record into that slot's snapshot; a repeat MAC refreshes its slot in place and
 * a full map drops the record. Slots freed by the tick reap are already zeroed,
 * so a reclaimed slot starts scoring from a clean warm-up. */
void wei_infer_stage_record(wei_infer_engine_t *eng, const uint8_t mac[6],
    const wei_conn_metric_record_t *rec)
{
    wei_infer_slot_t *slot;
    int64_t now_ms;
    unsigned int i;
    int free_slot = -1;

    if (eng == NULL || mac == NULL || rec == NULL) {
        return;
    }

    now_ms = wei_infer_now_ms();

    for (i = 0; i < WEI_TRACK_MAX_CLIENTS; i++) {
        slot = &eng->map.slots[i];
        if (slot->in_use) {
            if (memcmp(slot->mac, mac, sizeof(slot->mac)) == 0) {
                slot->last_seen = now_ms;
                eng->snapshot[i] = *rec;
                wei_util_dbg_print(WEI_CONNECTED,
                    "%s:%d [STAGE] refresh mac=%02x:%02x:%02x:%02x:%02x:%02x slot=%u\n",
                    __func__, __LINE__, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], i);
                return;
            }
        } else if (free_slot < 0) {
            free_slot = (int)i;
        }
    }

    if (free_slot < 0) {
        wei_util_error_print(WEI_CONNECTED,
            "%s:%d [STAGE] map FULL (%u) -> dropping mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
            __func__, __LINE__, (unsigned)eng->map.count,
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return;
    }

    slot = &eng->map.slots[free_slot];
    memcpy(slot->mac, mac, sizeof(slot->mac));
    slot->in_use = 1;
    slot->last_seen = now_ms;
    eng->snapshot[free_slot] = *rec;
    if (eng->map.count < WEI_TRACK_MAX_CLIENTS) {
        eng->map.count++;
    }

    wei_util_info_print(WEI_CONNECTED,
        "%s:%d [STAGE] admit mac=%02x:%02x:%02x:%02x:%02x:%02x slot=%d count=%u\n",
        __func__, __LINE__, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        free_slot, (unsigned)eng->map.count);
}

void wei_infer_destroy(wei_infer_engine_t *eng)
{
    free(eng);
}
