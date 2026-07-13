bin_PROGRAMS = wei

AM_CPPFLAGS = -I$(srcdir)/include \
              -I${PKG_CONFIG_SYSROOT_DIR}${includedir}/ccsp \
              -I${includedir}/rbus \
              -I${includedir}/ccsp/math_utils

wei_LDADD = -lrbus -lwifi_bus -lwifi_math_utils -lcjson -lm -lpthread -lstdc++

wei_SOURCES = source/wei_conn_scorer.c source/wei_connperf_dml.c source/wei_infer.c source/wei_life.c source/wei_perfreport.c source/wei_platform_cap.c source/wei_poll.c source/wei_report_publish.c source/wei_track.c source/wei_util.c source/weid_bus.c source/weid_daemon.c source/weid_rfc.c source/weid_wire.c
