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

#include "weid_wire.h"
#include "weid_receiver.h"
#include "wei_conn_metric.h"
#include "wei_util.h"

#include <stddef.h>
#include <string.h>

/* Caller-supplied scorer policy for the standalone daemon; the C4 engine bakes
 * none of these, so the daemon owns them here as wei_life owns its own default
 * policy. Expressed on the engine's own scales -- the 0-100 connected-P score
 * (LAT-2/BUF-1), the smoothed-PHY variance in (Mbps)^2 (TP-3), and the poll
 * cadence -- these are demo defaults pending a tuned source. */
#define WEID_WIRE_TPUT_VAR_BAND       25
#define WEID_WIRE_TPUT_FLOOR_MS       (2 * WEI_TICK_DEFAULT_MS)
#define WEID_WIRE_LATENCY_SCORE_BAND  40u
#define WEID_WIRE_LATENCY_SUSTAIN     3u
#define WEID_WIRE_VIDEO_DEGRADE_BAND  50u

static const wei_infer_policy_t weid_wire_default_policy = {
    .tput_var_band         = WEID_WIRE_TPUT_VAR_BAND,
    .tput_floor_ms         = WEID_WIRE_TPUT_FLOOR_MS,
    .latency_score_band    = WEID_WIRE_LATENCY_SCORE_BAND,
    .latency_sustain_ticks = WEID_WIRE_LATENCY_SUSTAIN,
    .video_degrade_band    = WEID_WIRE_VIDEO_DEGRADE_BAND,
};

/* Maps one raw per-station wire entry into the C1 metric record the engine
 * scores. SNR and the uplink PHY rate (converted from the wire's Mbps to the
 * record's kbps) and the retransmission count carry the link-quality signal;
 * activity comes from the client's active flag. Channel utilisation is a
 * radio-level percentage (0-100) carried on the top-level stats_arg_t
 * (run_qmgr.h), populated by the OneWifi linkquality stats path via
 * get_radio_channel_utilization() and fed to the C5 sigmoid de-weight; it is
 * clamped defensively into the record's 0-100 byte. */
static void weid_wire_map_record(const stats_arg_t *in, wei_conn_metric_record_t *out)
{
    memset(out, 0, sizeof(*out));
    out->version = WEI_CONN_METRIC_VERSION;
    out->activity_state = in->dev.cli_Active ? WEI_CONN_METRIC_STATE_ACTIVE
                                             : WEI_CONN_METRIC_STATE_IDLE;
    out->status = WEI_CONN_METRIC_STATUS_OK;
    out->link_snr_db = (int32_t)in->dev.cli_SNR;
    out->phy_rate_kbps = (uint32_t)in->dev.cli_LastDataUplinkRate * 1000u;
    out->pkt_err_rate = (uint16_t)in->dev.cli_Retransmissions;
    out->chan_util_pct = (uint8_t)(in->channel_utilization < 0 ? 0 :
            (in->channel_utilization > 100 ? 100 : in->channel_utilization));
}

/* C3 datagram callback on the single poll thread: reads the borrowed buffer as
 * the packed TLV, rejects a frame whose declared value length does not match the
 * received byte count, and for a periodic-stats frame stages each raw stats_arg_t
 * entry into the engine keyed by its MAC. Copies each entry out of the packed,
 * possibly unaligned wire buffer before use and retains nothing past return. */
static void weid_on_datagram(const uint8_t *buf, size_t len, void *user)
{
    weid_ctx_t *ctx = (weid_ctx_t *)user;
    const weid_tlv_t *tlv;
    size_t payload_len;
    size_t count;
    size_t i;

    if (ctx == NULL || buf == NULL || len < offsetof(weid_tlv_t, value)) {
        wei_util_error_print(WEI_CONNECTED,
            "%s:%d [IPC-RECV] drop: ctx/buf NULL or runt datagram (len=%zu min=%zu)\n",
            __func__, __LINE__, len, offsetof(weid_tlv_t, value));
        return;
    }

    tlv = (const weid_tlv_t *)buf;
    payload_len = len - offsetof(weid_tlv_t, value);

    /* Fail closed on any wire/struct drift, and log the exact reason so a dropped
     * datagram is never a silent mystery. */
    if (tlv->version != LQ_IPC_WIRE_VERSION) {
        wei_util_error_print(WEI_CONNECTED,
            "%s:%d [IPC-RECV] REJECT version=%u (want %u)\n",
            __func__, __LINE__, tlv->version, (unsigned)LQ_IPC_WIRE_VERSION);
        return;
    }
    if (tlv->elem_size != sizeof(stats_arg_t)) {
        wei_util_error_print(WEI_CONNECTED,
            "%s:%d [IPC-RECV] REJECT elem_size=%u want=%zu -- sender/daemon stats_arg_t drift\n",
            __func__, __LINE__, tlv->elem_size, sizeof(stats_arg_t));
        return;
    }
    if (tlv->len != payload_len) {
        wei_util_error_print(WEI_CONNECTED,
            "%s:%d [IPC-RECV] REJECT tlv.len=%u != payload=%zu\n",
            __func__, __LINE__, tlv->len, payload_len);
        return;
    }
    if (payload_len % sizeof(stats_arg_t) != 0) {
        wei_util_error_print(WEI_CONNECTED,
            "%s:%d [IPC-RECV] REJECT payload=%zu not a multiple of elem=%zu\n",
            __func__, __LINE__, payload_len, sizeof(stats_arg_t));
        return;
    }
    if (tlv->type != LQ_IPC_MSG_PERIODIC_STATS) {
        wei_util_dbg_print(WEI_CONNECTED,
            "%s:%d [IPC-RECV] ignore type=%u (daemon scores PERIODIC_STATS=%u only)\n",
            __func__, __LINE__, tlv->type, (unsigned)LQ_IPC_MSG_PERIODIC_STATS);
        return;
    }

    count = payload_len / sizeof(stats_arg_t);
    wei_util_info_print(WEI_CONNECTED,
        "%s:%d [IPC-RECV] PERIODIC_STATS ver=%u count=%zu bytes=%zu\n",
        __func__, __LINE__, tlv->version, count, len);

    for (i = 0; i < count; i++) {
        stats_arg_t entry;
        wei_conn_metric_record_t rec;
        const uint8_t *m;

        memcpy(&entry, tlv->value + i * sizeof(stats_arg_t), sizeof(entry));
        weid_wire_map_record(&entry, &rec);
        m = (const uint8_t *)entry.dev.cli_MACAddress;
        wei_util_dbg_print(WEI_CONNECTED,
            "%s:%d [IPC-RECV]  entry[%zu] mac=%02x:%02x:%02x:%02x:%02x:%02x "
            "snr=%d phy_kbps=%u pkt_err=%u chan_util=%u active=%d\n",
            __func__, __LINE__, i, m[0], m[1], m[2], m[3], m[4], m[5],
            rec.link_snr_db, rec.phy_rate_kbps, (unsigned)rec.pkt_err_rate,
            (unsigned)rec.chan_util_pct, (int)rec.activity_state);
        wei_infer_stage_record(ctx->engine, entry.dev.cli_MACAddress, &rec);
    }
}

/* C3 tick callback on the single poll thread: drives the engine's per-interval
 * sweep, which scores every staged client and publishes each report exactly once.
 * The publish lives inside the engine tick, so this callback never publishes and
 * cannot double-fire. */
static void weid_on_tick(void *user)
{
    weid_ctx_t *ctx = (weid_ctx_t *)user;

    if (ctx == NULL) {
        return;
    }

    wei_util_dbg_print(WEI_CONNECTED, "%s:%d [TICK] scoring sweep\n", __func__, __LINE__);
    wei_infer_tick(ctx->engine);
}

int weid_wire_init(weid_ctx_t *ctx)
{
    if (ctx == NULL) {
        return -1;
    }

    ctx->engine = wei_infer_create(&weid_wire_default_policy);
    if (ctx->engine == NULL) {
        wei_util_error_print(WEI_CONNECTED, "%s:%d wei_infer_create failed\n", __func__, __LINE__);
        return -1;
    }

    /* The poll's default cadence (WEI_TICK_DEFAULT_MS) is the required 5 s
     * interval, so no retune is needed. This is the one context bound to the
     * socket; the engine owns none. */
    ctx->poll = wei_poll_create(LQ_STATS_SOCKET_PATH, weid_on_datagram, weid_on_tick, ctx);
    if (ctx->poll == NULL) {
        wei_util_error_print(WEI_CONNECTED, "%s:%d wei_poll_create failed on %s\n",
            __func__, __LINE__, LQ_STATS_SOCKET_PATH);
        wei_infer_destroy(ctx->engine);
        ctx->engine = NULL;
        return -1;
    }

    wei_util_info_print(WEI_CONNECTED, "%s:%d receiver bound on %s (tick=%ums)\n",
        __func__, __LINE__, LQ_STATS_SOCKET_PATH, (unsigned)WEI_TICK_DEFAULT_MS);
    return 0;
}

void weid_wire_deinit(weid_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    wei_poll_stop(ctx->poll);
    ctx->poll = NULL;
    wei_infer_destroy(ctx->engine);
    ctx->engine = NULL;
}
