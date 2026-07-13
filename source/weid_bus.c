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

#include "wei_util.h"
#include "weid_bus.h"
#include "bus.h"
#include "wei_connperf_dml.h"

/* Fresh daemon bus component name, distinct from the OneWifi ctrl component so
 * the standalone daemon owns its own RBUS namespace. */
#define WEID_BUS_COMPONENT_NAME "wei"

/* The daemon's single bus handle. opened guards the accessor so a
 * null/uninitialised handle is never presented for publish and keeps open/close
 * idempotent. Touched only on the single daemon thread, matching the surrounding
 * wei_* units' no-new-lock convention. */
static struct {
    bus_handle_t handle;
    bool         opened;
} g_weid_bus = {
    .opened = false,
};

bus_error_t weid_bus_open(void)
{
    wifi_bus_desc_t *desc;
    bus_error_t rc;

    if (g_weid_bus.opened) {
        return bus_error_success;
    }

    /* wei is a separate process, so the loader does not fill g_bus.desc during
     * OneWifi init; call bus_init() ourselves to populate the bus descriptor
     * function-pointer table. */
    bus_init(NULL);

    desc = get_bus_descriptor();
    if (desc == NULL || desc->bus_open_fn == NULL || desc->bus_close_fn == NULL) {
        wei_util_error_print(WEI_CONNECTED, "%s:%d bus: descriptor or open/close fn is NULL\r\n",
            __func__, __LINE__);
        return bus_error_general;
    }

    rc = desc->bus_open_fn(&g_weid_bus.handle, WEID_BUS_COMPONENT_NAME);
    if (rc != bus_error_success) {
        wei_util_error_print(WEI_CONNECTED, "%s:%d bus: bus_open_fn failed for component:%s, rc:%d\r\n",
            __func__, __LINE__, WEID_BUS_COMPONENT_NAME, rc);
        return rc;
    }
    g_weid_bus.opened = true;

    rc = wei_connperf_dml_register();
    if (rc != bus_error_success) {
        wei_util_error_print(WEI_CONNECTED, "%s:%d bus: data-model registration failed, rc:%d\r\n",
            __func__, __LINE__, rc);
        desc->bus_close_fn(&g_weid_bus.handle);
        g_weid_bus.opened = false;
        return rc;
    }

    wei_util_info_print(WEI_CONNECTED, "%s:%d: wei daemon bus opened and registered\r\n",
        __func__, __LINE__);
    return bus_error_success;
}

void weid_bus_close(void)
{
    wifi_bus_desc_t *desc;

    if (!g_weid_bus.opened) {
        return;
    }

    desc = get_bus_descriptor();
    if (desc != NULL && desc->bus_close_fn != NULL) {
        desc->bus_close_fn(&g_weid_bus.handle);
    }
    g_weid_bus.opened = false;
}

bus_handle_t *weid_bus_handle(void)
{
    return g_weid_bus.opened ? &g_weid_bus.handle : NULL;
}
