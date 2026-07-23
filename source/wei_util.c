/* wei_util.c -- self-contained logging implementation. SPDX-License-Identifier: Apache-2.0 */
#include "wei_util.h"
#include <pthread.h>
#include <time.h>
static const char *wei_lvl_str(wei_log_level_t l){switch(l){case WEI_LOG_LVL_DEBUG:return "DBG";case WEI_LOG_LVL_INFO:return "INF";case WEI_LOG_LVL_ERROR:return "ERR";default:return "LOG";}}
static const char *wei_zone_str(wei_dbg_type_t m){switch(m){case WEI_CONNECTED:return "CONNECTED";case WEI_AFF:return "AFFINITY";case WEI_APPS:return "APPS";default:return "WEI";}}

/* Extensive on-box debugging: emit every level down to DEBUG unless a build
 * override raises the floor. */
#ifndef WEI_LOG_MIN_LEVEL
#define WEI_LOG_MIN_LEVEL WEI_LOG_LVL_DEBUG
#endif

/* Dedicated WEI debug log on tmpfs, separate from stdout/journald. Because /tmp is
 * RAM, the file is hard-capped: on reaching WEI_LOG_FILE_MAX_BYTES it is truncated
 * and restarted (single-file rotation) so extensive logging can never exhaust RAM.
 * One mutex serialises the lazy open, the size check and the write because log
 * calls arrive from both the poll thread and the bus-callback thread. */
#define WEI_LOG_FILE_PATH      "/tmp/wei_debug.log"
#define WEI_LOG_FILE_MAX_BYTES (8 * 1024 * 1024)

static pthread_mutex_t wei_log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE           *wei_log_fp;
static long            wei_log_bytes;

static void wei_util_file_write(const char *line, size_t len)
{
    pthread_mutex_lock(&wei_log_lock);
    if (wei_log_fp == NULL) {
        wei_log_fp = fopen(WEI_LOG_FILE_PATH, "w");
        wei_log_bytes = 0;
    } else if (wei_log_bytes >= WEI_LOG_FILE_MAX_BYTES) {
        wei_log_fp = freopen(WEI_LOG_FILE_PATH, "w", wei_log_fp); /* closes old stream; NULL on fail */
        wei_log_bytes = 0;
    }
    if (wei_log_fp != NULL && len > 0) {
        wei_log_bytes += (long)fwrite(line, 1, len, wei_log_fp);
        fflush(wei_log_fp);
    }
    pthread_mutex_unlock(&wei_log_lock);
}

void wei_util_print(wei_log_level_t level, wei_dbg_type_t module, const char *format, ...)
{
    struct timespec ts; struct tm tm_buf; char stamp[32];
    char line[1024];
    int off, avail, n;
    va_list ap;

    if (format == NULL || level < WEI_LOG_MIN_LEVEL) return;

    clock_gettime(CLOCK_REALTIME, &ts);
    if (localtime_r(&ts.tv_sec, &tm_buf) != NULL && strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm_buf) > 0)
        off = snprintf(line, sizeof(line), "%s.%03ld [wei][%s][%s] ", stamp, ts.tv_nsec/1000000L, wei_lvl_str(level), wei_zone_str(module));
    else
        off = snprintf(line, sizeof(line), "[wei][%s][%s] ", wei_lvl_str(level), wei_zone_str(module));
    if (off < 0) off = 0;
    if (off > (int)sizeof(line) - 1) off = (int)sizeof(line) - 1;

    avail = (int)sizeof(line) - off;
    va_start(ap, format);
    n = vsnprintf(line + off, (size_t)avail, format, ap);
    va_end(ap);
    if (n > 0) off += (n < avail) ? n : (avail - 1);

    fputs(line, stdout);
    fflush(stdout);
    wei_util_file_write(line, (size_t)off);
}
