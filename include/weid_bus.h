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

#ifndef WEID_BUS_H
#define WEID_BUS_H
#ifdef __cplusplus
extern "C" {
#endif

/* Platform bus abstraction, consumed by its exact declared names; this unit
 * redefines none of it. */
#include "bus_common.h"

/* Daemon bus lifecycle: owns the standalone daemon's single bus_handle_t. The
 * daemon opens it once at init and closes it once at deinit; it is the one
 * publish target the report path fires through. */

/* Opens the daemon bus handle once under the daemon component name and registers
 * the connected-performance data-model elements on it exactly once. Returns
 * bus_error_success once the handle is open and registered (or when already
 * opened); on any failure it leaves no handle open and returns the failing
 * bus_error_t. */
bus_error_t weid_bus_open(void);

/* Closes the daemon bus handle exactly once. A no-op when the handle was never
 * opened, so it is safe on every shutdown path. */
void weid_bus_close(void);

/* The single daemon bus handle the publish path fires through, or NULL before a
 * successful weid_bus_open and after weid_bus_close. */
bus_handle_t *weid_bus_handle(void);

#ifdef __cplusplus
}
#endif
#endif /* WEID_BUS_H */
