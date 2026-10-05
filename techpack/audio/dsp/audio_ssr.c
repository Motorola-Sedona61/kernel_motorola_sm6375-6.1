// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/remoteproc/qcom_rproc.h>
#include <soc/qcom/subsystem_notif.h>
#include "audio_ssr.h"

static char *audio_ssr_domains[] = {
	"adsp",
	"mpss"
};

struct audio_ssr_handle {
	struct notifier_block bridge;
	struct notifier_block *client;
	void *remote;
};

static int audio_ssr_notify(struct notifier_block *nb, unsigned long event,
			    void *data)
{
	struct audio_ssr_handle *handle =
		container_of(nb, struct audio_ssr_handle, bridge);
	unsigned long legacy;

	switch (event) {
	case QCOM_SSR_BEFORE_POWERUP:
		legacy = SUBSYS_BEFORE_POWERUP;
		break;
	case QCOM_SSR_AFTER_POWERUP:
		legacy = SUBSYS_AFTER_POWERUP;
		break;
	case QCOM_SSR_BEFORE_SHUTDOWN:
		legacy = SUBSYS_BEFORE_SHUTDOWN;
		break;
	case QCOM_SSR_AFTER_SHUTDOWN:
		legacy = SUBSYS_AFTER_SHUTDOWN;
		break;
	default:
		return NOTIFY_DONE;
	}
	return handle->client->notifier_call(handle->client, legacy, data);
}

void *audio_ssr_register(int domain_id, struct notifier_block *nb)
{
	struct audio_ssr_handle *handle;
	int ret;

	if (domain_id < 0 || domain_id >= AUDIO_SSR_DOMAIN_MAX || !nb)
		return ERR_PTR(-EINVAL);
	if (!IS_ENABLED(CONFIG_QCOM_RPROC_COMMON))
		return ERR_PTR(-ENODEV);
	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return ERR_PTR(-ENOMEM);
	handle->client = nb;
	handle->bridge.notifier_call = audio_ssr_notify;
	handle->bridge.priority = nb->priority;
	handle->remote = qcom_register_ssr_notifier(audio_ssr_domains[domain_id],
					     &handle->bridge);
	if (IS_ERR_OR_NULL(handle->remote)) {
		ret = handle->remote ? PTR_ERR(handle->remote) : -ENODEV;
		kfree(handle);
		return ERR_PTR(ret);
	}
	return handle;
}
EXPORT_SYMBOL(audio_ssr_register);

int audio_ssr_deregister(void *opaque, struct notifier_block *nb)
{
	struct audio_ssr_handle *handle = opaque;
	int ret;

	if (IS_ERR_OR_NULL(handle) || handle->client != nb)
		return -EINVAL;
	ret = qcom_unregister_ssr_notifier(handle->remote, &handle->bridge);
	if (!ret)
		kfree(handle);
	return ret;
}
EXPORT_SYMBOL(audio_ssr_deregister);
