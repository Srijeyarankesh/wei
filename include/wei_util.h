/* wei_util.h -- self-contained logging for the standalone WEI daemon.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef WEI_UTIL_H
#define WEI_UTIL_H
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { WEI_CONNECTED, WEI_AFF, WEI_APPS } wei_dbg_type_t;
typedef enum { WEI_LOG_LVL_DEBUG, WEI_LOG_LVL_INFO, WEI_LOG_LVL_ERROR, WEI_LOG_LVL_MAX } wei_log_level_t;
void wei_util_print(wei_log_level_t level, wei_dbg_type_t module, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
#define wei_util_dbg_print(module, format, ...)  wei_util_print(WEI_LOG_LVL_DEBUG, (module), (format), ##__VA_ARGS__)
#define wei_util_info_print(module, format, ...) wei_util_print(WEI_LOG_LVL_INFO,  (module), (format), ##__VA_ARGS__)
#define wei_util_error_print(module, format, ...) wei_util_print(WEI_LOG_LVL_ERROR, (module), (format), ##__VA_ARGS__)
#ifdef __cplusplus
}
#endif
#endif /* WEI_UTIL_H */
