/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _FOGOS_AUDIO_REMOTEPROC_H
#define _FOGOS_AUDIO_REMOTEPROC_H
#include <linux/device.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/remoteproc.h>

static inline struct rproc *audio_rproc_get_boot(struct device *dev,
		const char *property, const char *firmware)
{
	struct rproc *rproc;
	u32 handle;
	int ret;

	ret = of_property_read_u32(dev->of_node, property, &handle);
	if (ret)
		return ERR_PTR(ret);
	rproc = rproc_get_by_phandle(handle);
	if (!rproc)
		return ERR_PTR(-EPROBE_DEFER);
	if (firmware) {
		ret = rproc_set_firmware(rproc, firmware);
		if (ret)
			goto put;
	}
	ret = rproc_boot(rproc);
	if (ret)
		goto put;
	return rproc;
put:
	rproc_put(rproc);
	return ERR_PTR(ret);
}

static inline void audio_rproc_shutdown_put(struct rproc *rproc)
{
	if (!IS_ERR_OR_NULL(rproc)) {
		rproc_shutdown(rproc);
		rproc_put(rproc);
	}
}
#endif
