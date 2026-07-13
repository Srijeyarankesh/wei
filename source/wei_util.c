/* wei_util.c -- self-contained logging implementation. SPDX-License-Identifier: Apache-2.0 */
#include "wei_util.h"
#include <time.h>
static const char *wei_lvl_str(wei_log_level_t l){switch(l){case WEI_LOG_LVL_DEBUG:return "DBG";case WEI_LOG_LVL_INFO:return "INF";case WEI_LOG_LVL_ERROR:return "ERR";default:return "LOG";}}
static const char *wei_zone_str(wei_dbg_type_t m){switch(m){case WEI_CONNECTED:return "CONNECTED";case WEI_AFF:return "AFFINITY";case WEI_APPS:return "APPS";default:return "WEI";}}
#ifndef WEI_LOG_MIN_LEVEL
#define WEI_LOG_MIN_LEVEL WEI_LOG_LVL_INFO
#endif
void wei_util_print(wei_log_level_t level, wei_dbg_type_t module, const char *format, ...)
{
    struct timespec ts; struct tm tm_buf; char stamp[32];
    if (format == NULL || level < WEI_LOG_MIN_LEVEL) return;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (localtime_r(&ts.tv_sec, &tm_buf) != NULL && strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf) > 0)
        printf("%s.%03ld [wei][%s][%s] ", stamp, ts.tv_nsec/1000000L, wei_lvl_str(level), wei_zone_str(module));
    else
        printf("[wei][%s][%s] ", wei_lvl_str(level), wei_zone_str(module));
    va_list ap; va_start(ap, format); vprintf(format, ap); va_end(ap); fflush(stdout);
}
