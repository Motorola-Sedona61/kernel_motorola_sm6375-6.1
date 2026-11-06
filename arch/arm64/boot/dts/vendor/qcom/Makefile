# SPDX-License-Identifier: GPL-2.0-only
# Fogos and bangkk share the Blair base; their board overlays are packaged
# into separate DTBO images.
dtb-$(CONFIG_FOGOS_DTB) += blair-moto-fogos-base.dtb
dtbo-$(CONFIG_FOGOS_DTB) += blair-fogos-evt-overlay.dtbo blair-fogos-dvt1-overlay.dtbo
DTC_FLAGS_blair-moto-fogos-base += -@
dtb-$(CONFIG_BANGKK_DTB) += blair-moto-fogos-base.dtb
dtbo-$(CONFIG_BANGKK_DTB) += blair-bangkk-evb1-overlay.dtbo
