SUMMARY = "WiFi Experience Index connected-performance scorer daemon"

LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=462b07363459f8aae8f120a394606fba"

DEPENDS = "rbus ccsp-one-wifi cjson"

SRC_URI = "git://github.com/Srijeyarankesh/wei.git;protocol=https;branch=develop;name=wei"

SRCREV_wei = "${AUTOREV}"
SRCREV_FORMAT = "wei"

PV = "${RDK_RELEASE}+git${SRCPV}"

S = "${WORKDIR}/git"

inherit autotools pkgconfig systemd

do_install_append () {
    install -d ${D}${exec_prefix}/ccsp/wei
    install -m 0755 ${S}/scripts/wei_start ${D}${exec_prefix}/ccsp/wei/wei_start

    install -d ${D}${systemd_unitdir}/system
    install -m 0644 ${S}/wei.service ${D}${systemd_unitdir}/system/wei.service
}

SYSTEMD_SERVICE_${PN} = "wei.service"

FILES_${PN} = " \
    ${bindir}/wei \
    ${exec_prefix}/ccsp/wei/wei_start \
    ${systemd_unitdir}/system/wei.service \
"
