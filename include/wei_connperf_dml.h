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

#ifndef WEI_CONNPERF_DML_H
#define WEI_CONNPERF_DML_H
#ifdef __cplusplus
extern "C" {
#endif

/* Platform bus abstraction, consumed by its exact declared names; this unit
 * redefines none of it. */
#include "bus_common.h"

/* Fresh transposed data-model full-name paths for the connected-performance
 * pillar surface: the boolean enable property, the uint32 configuration-flags
 * property, and the report/status event element the publish path fires through.
 * The unit registers these; it does not fire the event. */
#define WEI_CONNPERF_DM_ENABLE       "Device.WiFi.X_RDK_ConnPerf.Enable"
#define WEI_CONNPERF_DM_CONFIG_FLAGS "Device.WiFi.X_RDK_ConnPerf.ConfigFlags"
#define WEI_CONNPERF_DM_REPORT_EVENT "Device.WiFi.X_RDK_ConnPerf.Report"

/* Opaque module config-state holder; its layout lives in wei_connperf_dml.c. */
typedef struct wei_connperf_dml_state wei_connperf_dml_state_t;

/* Registers the connected-performance data-model elements on the daemon's shared
 * bus handle exactly once. The handle MUST be the same one the report publish path
 * fires through (weid_bus_handle), because rbus requires an event to be published
 * on the very handle that registered it. Returns bus_error_success on registration
 * (or when already registered), and the failing bus_error_t otherwise. */
bus_error_t wei_connperf_dml_register(bus_handle_t *handle);

#ifdef __cplusplus
}
#endif
#endif /* WEI_CONNPERF_DML_H */
