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

#ifndef WEI_LIFE_H
#define WEI_LIFE_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <pthread.h>

/* Consumed contracts, bound by their exact declared names: the C1 per-client
 * metric record this unit reads, and the C3 crossing-with-duration tracker and
 * interval tick it delegates all excursion timing to. This unit redefines none
 * of them. */
#include "wei_conn_metric.h"
#include "wei_track.h"
#include "wei_poll.h"

/* Roster capacity tracks the C3 tracker's fixed client bound so a station that
 * can be timed always has a slot. */
#define WEI_LIFE_MAX_STATIONS WEI_TRACK_MAX_CLIENTS

/* Station connection-lifecycle event delivered to the unit from the existing
 * app-framework/HAL event surface. */
typedef enum {
    WEI_LIFE_EVENT_ASSOC = 0,
    WEI_LIFE_EVENT_DISASSOC,
    WEI_LIFE_EVENT_CONN_CHANGE
} wei_life_event_t;

/* Which per-client metric an excursion belongs to. */
typedef enum {
    WEI_LIFE_METRIC_SNR = 0,
    WEI_LIFE_METRIC_LOSS,
    WEI_LIFE_METRIC_PHY,
    WEI_LIFE_METRIC_CHANUTIL,
    WEI_LIFE_METRIC_COUNT
} wei_life_metric_t;

/* Configurable bound + dwell pair per metric, supplied to the C3 tracker as its
 * band / floor_ms policy inputs; defaults ship but stay overridable. */
typedef struct {
    int32_t snr_floor;
    int64_t snr_dwell_ms;
    int32_t loss_ceiling;
    int64_t loss_dwell_ms;
    int32_t phy_floor;
    int64_t phy_dwell_ms;
    int32_t chanutil_ceiling;
    int64_t chanutil_dwell_ms;
} wei_life_policy_t;

/* One accumulated excursion handed toward the connected scorer path. */
typedef struct {
    uint8_t           mac[6];
    wei_life_metric_t metric;
    uint32_t          onset_count;
    int64_t           duration_ms;
} wei_life_excursion_t;

/* Downstream sink for a per-station excursion, delivered with the station's
 * current connected-P score. The integrator registers the concrete stage
 * (scorer aggregation / publish); the unit owns no downstream sink of its own. */
typedef void (*wei_life_report_fn)(void *user, const wei_life_excursion_t *excursion,
    uint8_t connected_score);

/* Per-station tracking context, opened on attach and torn down on detach.
 * Opaque: internals live in wei_life.c. */
typedef struct wei_life_ctx wei_life_ctx_t;

/* The unit's own tracked-set: per-station contexts guarded by a single lock,
 * plus the excursion policy. Distinct from the core associated-devices map. */
typedef struct {
    pthread_mutex_t   guard;
    wei_life_ctx_t   *slots[WEI_LIFE_MAX_STATIONS];
    unsigned int      active;
    wei_life_policy_t policy;
} wei_life_roster_t;

int  wei_life_init(wei_life_roster_t *roster, const wei_life_policy_t *policy);
void wei_life_deinit(wei_life_roster_t *roster);

void wei_life_on_station_event(wei_life_roster_t *roster, wei_life_event_t event,
    const uint8_t mac[6]);

/* Deposits a station's latest C1 metric record into its tracking context, read
 * back by the interval detectors; an untracked station is ignored. */
void wei_life_ingest_metric(wei_life_roster_t *roster, const uint8_t mac[6],
    const wei_conn_metric_record_t *record);

wei_life_ctx_t *wei_life_attach_station(wei_life_roster_t *roster, const uint8_t mac[6]);
void            wei_life_detach_station(wei_life_roster_t *roster, const uint8_t mac[6]);

/* Registers the downstream excursion sink; passing NULL sink detaches it. */
void wei_life_set_report_sink(wei_life_report_fn sink, void *user);

/* C3 wei_poll_on_tick callback: user is the wei_life_roster_t. */
void wei_life_eval_station(void *user);

/* Yields the unit's tick callback for the single upstream poll daemon to install
 * as its wei_poll_on_tick; the unit creates no poll loop and spawns no thread.
 * The wei_poll_on_tick return type fails this build on any signature drift. */
wei_poll_on_tick wei_life_tick_fn(void);

#ifdef __cplusplus
}
#endif
#endif /* WEI_LIFE_H */
