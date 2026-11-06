# SPDX-License-Identifier: GPL-2.0-only
# Fogos, bangkk and rhodep share the Blair base; their board overlays are
# packaged into separate DTBO images. Rhodep's base drops the WLAN chain1
# and L21 proxy supplies at staging time (L21 is its camera rail).
dtb-$(CONFIG_FOGOS_DTB) += blair-moto-fogos-base.dtb
dtbo-$(CONFIG_FOGOS_DTB) += blair-fogos-evt-overlay.dtbo blair-fogos-dvt1-overlay.dtbo
DTC_FLAGS_blair-moto-fogos-base += -@
dtb-$(CONFIG_BANGKK_DTB) += blair-moto-fogos-base.dtb
dtbo-$(CONFIG_BANGKK_DTB) += blair-bangkk-evb1-overlay.dtbo
dtb-$(CONFIG_RHODEP_DTB) += blair-moto-fogos-base.dtb
dtbo-$(CONFIG_RHODEP_DTB) += blair-rhodep-evt1-overlay.dtbo blair-rhodep-evt2-overlay.dtbo \
	blair-rhodep-dvt1-overlay.dtbo blair-rhodep-dvt2-overlay.dtbo
