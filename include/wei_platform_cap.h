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

#ifndef WEI_PLATFORM_CAP_H
#define WEI_PLATFORM_CAP_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* Compile-time supported-platform descriptor for the connected-performance
 * feature. A downstream consumer consults wei_plat_tier_supported() before
 * activating the feature, so it runs only on a platform tier this pillar
 * supports. The query is a pure, total function of its argument: it issues no
 * HAL / nl80211 / hostapd / bus call, probes no environment, acquires no lock,
 * allocates nothing, and carries no state between calls. Every tier value --
 * recognised, unknown, or a future tier not yet present in the table -- has a
 * defined verdict, with unknown / unrecognised / future resolving to
 * unsupported cleanly. This unit describes platform support only; it neither
 * reads feature-enable state nor touches any platform-lifecycle surface. */

/* Supported-platform tiers, append-only: a further tier is one appended
 * enumerator plus one appended descriptor row, never a query-logic change.
 * WEI_PLAT_TIER_UNKNOWN is the explicit unrecognised sentinel and stays first;
 * WEI_PLAT_TIER_MAX bounds the descriptor table and stays last. */
typedef enum {
    WEI_PLAT_TIER_UNKNOWN = 0,
    WEI_PLAT_TIER_XB8,
    WEI_PLAT_TIER_XB7,
    WEI_PLAT_TIER_MAX
} wei_plat_tier_t;

/* One descriptor row: a tier identity paired with whether the feature is
 * supported on it. XB8+ ships supported (MVP); XB7+ ships present-but-reserved,
 * so enabling it post-MVP is a single-field edit to its row. */
typedef struct {
    wei_plat_tier_t tier;
    bool            supported;
} wei_plat_tier_desc_t;

/* Returns true when the feature is supported on the given tier, false otherwise.
 * Total and crash-safe: the unknown sentinel, any unrecognised value, and any
 * out-of-range or future tier resolve to false via a bounds-checked, table-driven
 * lookup; no branch is left undefined. */
bool wei_plat_tier_supported(wei_plat_tier_t tier);

#ifdef __cplusplus
}
#endif
#endif /* WEI_PLATFORM_CAP_H */
