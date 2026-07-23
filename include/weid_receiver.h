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

#ifndef WEID_RECEIVER_H
#define WEID_RECEIVER_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "run_qmgr.h"

/* Daemon-local copy of the OneWifi linkquality sender's AF_UNIX SOCK_DGRAM wire
 * contract. The type code values, the TLV layout and the socket path are
 * transposed from that sender and MUST stay byte-for-byte equal to it: a
 * mismatch corrupts the IPC silently, with no compile error. The sender's
 * private IPC header is not staged to the sysroot, so it is deliberately not
 * pulled in; the per-station payload stats_arg_t is consumed by exact name from
 * the staged run_qmgr.h and never redefined here. Same-box loopback, host byte
 * order. */

/* Value-parity checklist (Q5): every constant below MUST equal OneWifi's
 * linkquality sender. lq_ipc_sender.h is not staged to the sysroot, so parity is
 * held by review against the sender subsystem at
 * ccsp-one-wifi/source/apps/linkquality/ (same subsystem as wifi_linkquality.c);
 * the payload struct is the staged stats_arg_t at run_qmgr.h:55. Re-verify all of
 * these against the sender if it changes -- a mismatch corrupts the IPC silently.
 * The full code space is carried so an unhandled type is ignored rather than
 * misread as a handled one; the daemon acts only on PERIODIC_STATS, DISCONNECT,
 * RAPID_DISCONNECT, REGISTER_STA and UNREGISTER_STA:
 *   LQ_STATS_SOCKET_PATH         "/tmp/linkquality_stats.sock"
 *   LQ_IPC_WIRE_VERSION          1
 *   LQ_IPC_MSG_PERIODIC_STATS    1
 *   LQ_IPC_MSG_DISCONNECT        2
 *   LQ_IPC_MSG_RAPID_DISCONNECT  3
 *   LQ_IPC_MSG_CAFFINITY_EVENT   4
 *   LQ_IPC_MSG_START_METRICS     5
 *   LQ_IPC_MSG_STOP_METRICS      6
 *   LQ_IPC_MSG_REGISTER_STA      7
 *   LQ_IPC_MSG_UNREGISTER_STA    8
 *   LQ_IPC_MSG_REINIT_METRICS    9
 *   LQ_IPC_MSG_SET_MAX_SNR       10
 *   LQ_IPC_MSG_SET_SCORE_PARAMS  11
 *   per-station payload          array of stats_arg_t, read raw (no reserialize) */

#define LQ_STATS_SOCKET_PATH "/tmp/linkquality_stats.sock"

/* Wire-format version. MUST equal OneWifi lq_ipc_sender.h's LQ_IPC_WIRE_VERSION.
 * Bump on any change to the TLV framing or on-wire payload meaning; the receiver
 * rejects a datagram whose version it does not recognise. */
#define LQ_IPC_WIRE_VERSION 1u

enum {
    LQ_IPC_MSG_PERIODIC_STATS   = 1,
    LQ_IPC_MSG_DISCONNECT       = 2,
    LQ_IPC_MSG_RAPID_DISCONNECT = 3,
    LQ_IPC_MSG_CAFFINITY_EVENT  = 4,
    LQ_IPC_MSG_START_METRICS    = 5,
    LQ_IPC_MSG_STOP_METRICS     = 6,
    LQ_IPC_MSG_REGISTER_STA     = 7,
    LQ_IPC_MSG_UNREGISTER_STA   = 8,
    LQ_IPC_MSG_REINIT_METRICS   = 9,
    LQ_IPC_MSG_SET_MAX_SNR      = 10,
    LQ_IPC_MSG_SET_SCORE_PARAMS = 11
};

/* One datagram is one versioned TLV: a one-byte type, a one-byte wire version, a
 * two-byte element size, a two-byte value length, then the packed payload. Packed
 * so value begins at byte 6 with no alignment padding, matching the sender's
 * framing exactly. elem_size carries the sender's sizeof(stats_arg_t) so a struct
 * drift between the two builds is rejected rather than misparsed. */
typedef struct {
    uint8_t  type;
    uint8_t  version;
    uint16_t elem_size;
    uint16_t len;
    uint8_t  value[];
} __attribute__((packed)) weid_tlv_t;

/* Compile-time parity guard (Q5). The daemon carries its own copy of the sender's
 * TLV framing, so a mismatch is silent on the wire -- pin it to a build break
 * instead: value[] must start at byte 6 (1-byte type + 1-byte version + 2-byte
 * elem_size + 2-byte len, no padding), and the 6-byte MAC the parser lifts from
 * the staged stats_arg_t and keys the engine on must not change width.
 * sizeof(stats_arg_t) is deliberately not pinned here -- it is the shared staged
 * struct (parity by construction) and is instead checked at runtime against the
 * sender's elem_size field. */
_Static_assert(offsetof(weid_tlv_t, value) == 6,
               "weid_tlv_t must frame value[] at byte 6 to match the linkquality sender");
_Static_assert(sizeof(weid_tlv_t) == 6,
               "weid_tlv_t header must be packed to 6 bytes with no padding");
_Static_assert(sizeof(((stats_arg_t *)0)->dev.cli_MACAddress) == 6,
               "stats_arg_t MAC key must stay 6 bytes");

#ifdef __cplusplus
}
#endif
#endif /* WEID_RECEIVER_H */
