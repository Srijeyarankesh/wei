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
#include "wei_connperf_dml.h"
#include "bus.h"
#include "weid_rfc.h"

/* Backing state for the connected-performance data-model surface. registered
 * keeps wei_connperf_dml_register one-time; config_flags is an opaque uint32
 * pass-through whose bit meaning a consuming lane assigns later, so this unit
 * imposes no bit schema on it. */
struct wei_connperf_dml_state {
    bool         registered;
    uint32_t     config_flags;
    bus_handle_t handle;
};

static wei_connperf_dml_state_t g_wei_connperf_dml = {
    .registered   = false,
    .config_flags = 0,
};

/* The pillar enable is its own boolean on the shared global config, gated by no
 * other feature's flag. Like the surrounding DML accessors it is read/written
 * directly on the single ctrl thread with no explicit lock: the field is an
 * atomic-width bool and this unit adds no new lock. */
bus_error_t wei_connperf_enable_get(char *name, raw_data_t *p_data, bus_user_data_t *user_data)
{
    (void)user_data;

    if (name == NULL || p_data == NULL) {
        wei_util_error_print(WEI_CONNECTED, "%s:%d property name is not found\r\n", __func__, __LINE__);
        return bus_error_invalid_input;
    }

    p_data->data_type = bus_data_type_boolean;
    p_data->raw_data.b = weid_rfc_connperf_enabled();

    return bus_error_success;
}

bus_error_t wei_connperf_enable_set(char *name, raw_data_t *p_data, bus_user_data_t *user_data)
{
    (void)user_data;

    if (name == NULL || p_data == NULL) {
        wei_util_error_print(WEI_CONNECTED, "%s:%d property name is not found\r\n", __func__, __LINE__);
        return bus_error_invalid_input;
    }

    if (p_data->data_type != bus_data_type_boolean) {
        wei_util_error_print(WEI_CONNECTED, "%s:%d-%s wrong bus data_type:%x\n", __func__, __LINE__,
            name, p_data->data_type);
        return bus_error_invalid_input;
    }

    if (weid_rfc_connperf_set(p_data->raw_data.b) != 0) {
        wei_util_error_print(WEI_CONNECTED, "%s:%d failed to persist connperf enable\r\n", __func__,
            __LINE__);
        return bus_error_general;
    }

    return bus_error_success;
}

bus_error_t wei_connperf_config_get(char *name, raw_data_t *p_data, bus_user_data_t *user_data)
{
    (void)user_data;

    if (name == NULL || p_data == NULL) {
        wei_util_error_print(WEI_CONNECTED, "%s:%d property name is not found\r\n", __func__, __LINE__);
        return bus_error_invalid_input;
    }

    p_data->data_type = bus_data_type_uint32;
    p_data->raw_data.u32 = g_wei_connperf_dml.config_flags;

    return bus_error_success;
}

bus_error_t wei_connperf_config_set(char *name, raw_data_t *p_data, bus_user_data_t *user_data)
{
    (void)user_data;

    if (name == NULL || p_data == NULL) {
        wei_util_error_print(WEI_CONNECTED, "%s:%d property name is not found\r\n", __func__, __LINE__);
        return bus_error_invalid_input;
    }

    if (p_data->data_type != bus_data_type_uint32) {
        wei_util_error_print(WEI_CONNECTED, "%s:%d-%s wrong bus data_type:%x\n", __func__, __LINE__,
            name, p_data->data_type);
        return bus_error_invalid_input;
    }

    g_wei_connperf_dml.config_flags = p_data->raw_data.u32;

    return bus_error_success;
}

/* Subscription callback for the report/status event element. It only
 * acknowledges subscribe/unsubscribe intent; the publish path (a separate lane)
 * owns firing any value through this element. */
bus_error_t wei_connperf_event_sub(char *event_name, bus_event_sub_action_t action, int32_t interval,
    bool *auto_publish)
{
    (void)auto_publish;
    wei_util_dbg_print(WEI_CONNECTED, "%s:%d event:%s action:%d interval:%d\r\n", __func__, __LINE__,
        event_name ? event_name : "(null)", action, interval);

    return bus_error_success;
}

/* Register-once entry: opens the unit's own bus component and registers the
 * three connected-performance elements (enable + config-flags properties and
 * the report/status event) on it. The event element is registered here only so
 * the publish lane has a target to fire through; this unit fires nothing. */
bus_error_t wei_connperf_dml_register(void)
{
    char *component_name = "WifiConnPerf";
    uint32_t num_elements;
    bus_error_t rc;

    bus_data_element_t data_elements[] = {
        { WEI_CONNPERF_DM_ENABLE, bus_element_type_property,
            { wei_connperf_enable_get, wei_connperf_enable_set, NULL, NULL, NULL, NULL }, slow_speed,
            ZERO_TABLE, { bus_data_type_boolean, true, 0, 0, 0, NULL } },
        { WEI_CONNPERF_DM_CONFIG_FLAGS, bus_element_type_property,
            { wei_connperf_config_get, wei_connperf_config_set, NULL, NULL, NULL, NULL }, slow_speed,
            ZERO_TABLE, { bus_data_type_uint32, true, 0, 0, 0, NULL } },
        { WEI_CONNPERF_DM_REPORT_EVENT, bus_element_type_event,
            { NULL, NULL, NULL, NULL, wei_connperf_event_sub, NULL }, slow_speed,
            ZERO_TABLE, { bus_data_type_bytes, false, 0, 0, 0, NULL } },
    };

    if (g_wei_connperf_dml.registered) {
        return bus_error_success;
    }

    if (get_bus_descriptor() == NULL || get_bus_descriptor()->bus_open_fn == NULL) {
        wei_util_error_print(WEI_CONNECTED, "%s:%d bus: descriptor or bus_open_fn is NULL\r\n",
            __func__, __LINE__);
        return bus_error_general;
    }

    rc = get_bus_descriptor()->bus_open_fn(&g_wei_connperf_dml.handle, component_name);
    if (rc != bus_error_success) {
        wei_util_error_print(WEI_CONNECTED, "%s:%d bus: bus_open_fn open failed for component:%s, rc:%d\r\n",
            __func__, __LINE__, component_name, rc);
        return rc;
    }

    num_elements = (sizeof(data_elements) / sizeof(bus_data_element_t));
    rc = get_bus_descriptor()->bus_reg_data_element_fn(&g_wei_connperf_dml.handle, data_elements,
        num_elements);
    if (rc != bus_error_success) {
        wei_util_error_print(WEI_CONNECTED, "%s:%d bus: bus_reg_data_element_fn failed, rc:%d\r\n",
            __func__, __LINE__, rc);
        return rc;
    }

    g_wei_connperf_dml.registered = true;
    wei_util_info_print(WEI_CONNECTED, "%s:%d: wei connperf data-model elements registered\r\n",
        __func__, __LINE__);

    return bus_error_success;
}
