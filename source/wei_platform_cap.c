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

#include "wei_platform_cap.h"

/* One descriptor row per platform tier. XB8+ is the live MVP tier; XB7+ ships
 * present-but-reserved, so enabling it post-MVP is flipping this row's supported
 * field. A further tier is one appended enumerator plus one appended row here --
 * the table-driven lookup below is unchanged. */
static const wei_plat_tier_desc_t wei_plat_tier_table[] = {
    { WEI_PLAT_TIER_UNKNOWN, false },
    { WEI_PLAT_TIER_XB8,     true  },
    { WEI_PLAT_TIER_XB7,     false },
};

bool wei_plat_tier_supported(wei_plat_tier_t tier)
{
    unsigned int i;

    for (i = 0; i < sizeof(wei_plat_tier_table) / sizeof(wei_plat_tier_table[0]); i++) {
        if (wei_plat_tier_table[i].tier == tier) {
            return wei_plat_tier_table[i].supported;
        }
    }
    return false;
}
