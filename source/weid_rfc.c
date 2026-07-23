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

#include "weid_rfc.h"
#include "wei_util.h"

#include <cjson/cJSON.h>

#include <stddef.h>
#include <stdio.h>

/* Fresh daemon-owned path, distinct from OneWifi's config-of-record. */
#define WEID_RFC_NVRAM_PATH   "/nvram/wei.json"

/* Bounds the one-shot config read and write so a malformed or oversized file
 * can never drive an unbounded read; a valid RFC document is a few dozen bytes. */
#define WEID_RFC_BUF_MAX      4096u

/* Nested object + boolean key transposed from the real daemon's wei.json: the
 * connected-performance pillar is the "when connected" RFC flag. */
#define WEID_RFC_OBJECT       "RFC"
#define WEID_RFC_CONNPERF     "WhenConnected"

/* Cached RFC state, seeded to the safe default (feature DISABLED) so every
 * accessor call is well-defined before weid_rfc_load and after any load
 * failure. Read/written on the single daemon thread, matching the surrounding
 * wei_* units' no-new-lock convention. */
static struct {
    bool connperf_enabled;
} g_weid_rfc = {
    .connperf_enabled = false,
};

static size_t weid_rfc_read_file(char *buf, size_t cap)
{
    FILE *fp;
    size_t n;

    fp = fopen(WEID_RFC_NVRAM_PATH, "r");
    if (fp == NULL) {
        return 0;
    }

    n = fread(buf, 1, cap - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return n;
}

void weid_rfc_load(void)
{
    char buf[WEID_RFC_BUF_MAX];
    cJSON *doc;
    cJSON *flag;

    g_weid_rfc.connperf_enabled = false;

    if (weid_rfc_read_file(buf, sizeof(buf)) == 0) {
        wei_util_info_print(WEI_CONNECTED,
            "%s:%d RFC file %s absent/empty -> connperf DISABLED (default)\n",
            __func__, __LINE__, WEID_RFC_NVRAM_PATH);
        return;
    }

    doc = cJSON_Parse(buf);
    if (doc == NULL) {
        wei_util_error_print(WEI_CONNECTED,
            "%s:%d RFC file %s is malformed JSON -> connperf DISABLED\n",
            __func__, __LINE__, WEID_RFC_NVRAM_PATH);
        return;
    }

    flag = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(doc, WEID_RFC_OBJECT), WEID_RFC_CONNPERF);
    if (cJSON_IsBool(flag)) {
        g_weid_rfc.connperf_enabled = cJSON_IsTrue(flag) ? true : false;
    }
    wei_util_info_print(WEI_CONNECTED,
        "%s:%d RFC loaded from %s: %s.%s=%s\n",
        __func__, __LINE__, WEID_RFC_NVRAM_PATH, WEID_RFC_OBJECT, WEID_RFC_CONNPERF,
        g_weid_rfc.connperf_enabled ? "true" : "false");

    cJSON_Delete(doc);
}

bool weid_rfc_connperf_enabled(void)
{
    return g_weid_rfc.connperf_enabled;
}

int weid_rfc_connperf_set(bool enabled)
{
    char text[WEID_RFC_BUF_MAX];
    cJSON *doc;
    cJSON *rfc;
    FILE *fp;
    bool ok;

    g_weid_rfc.connperf_enabled = enabled;

    doc = cJSON_CreateObject();
    if (doc == NULL) {
        return -1;
    }
    rfc = cJSON_AddObjectToObject(doc, WEID_RFC_OBJECT);
    if (rfc == NULL || cJSON_AddBoolToObject(rfc, WEID_RFC_CONNPERF, enabled) == NULL) {
        cJSON_Delete(doc);
        return -1;
    }

    ok = cJSON_PrintPreallocated(doc, text, (int)sizeof(text), 0) ? true : false;
    cJSON_Delete(doc);
    if (!ok) {
        return -1;
    }

    fp = fopen(WEID_RFC_NVRAM_PATH, "w");
    if (fp == NULL) {
        return -1;
    }
    ok = (fputs(text, fp) >= 0);
    if (fclose(fp) != 0) {
        ok = false;
    }

    return ok ? 0 : -1;
}
