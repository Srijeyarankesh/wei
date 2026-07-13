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

#ifndef WEID_RFC_H
#define WEID_RFC_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/* Daemon-owned RFC/config store: the standalone daemon's enable-of-record for
 * the connected-performance pillar, cached from a daemon-owned nvram JSON at a
 * fresh path distinct from OneWifi's config-of-record. Consulted in place of
 * g_wifidb so the daemon links without any cross-process symbol. */

/* Loads the daemon RFC config from the nvram JSON into the local cache. On an
 * absent, corrupt, or truncated file the cache falls back to the safe default
 * (feature DISABLED); this never crashes and reads no more than a bounded
 * buffer. */
void weid_rfc_load(void);

/* Pure accessor for the connected-performance enable, valid before any load
 * (returns the DISABLED default) and after any load. */
bool weid_rfc_connperf_enabled(void);

/* Updates the cached connected-performance enable and persists it to the nvram
 * JSON. Returns 0 on success, -1 if the value could not be written back. */
int weid_rfc_connperf_set(bool enabled);

#ifdef __cplusplus
}
#endif
#endif /* WEID_RFC_H */
