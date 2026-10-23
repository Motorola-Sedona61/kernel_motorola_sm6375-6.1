// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2016-2017, The Linux Foundation. All rights reserved. */
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/soc/qcom/pdr.h>
#include "audio_pdr.h"

SRCU_NOTIFIER_HEAD_STATIC(audio_pdr_cb_list);
SRCU_NOTIFIER_HEAD_STATIC(audio_pdr_service_list);
static struct pdr_handle *audio_pdr;
static int audio_pdr_state = SERVREG_SERVICE_STATE_DOWN;
static bool audio_pdr_ready;

static void audio_pdr_status(int state, char *path, void *priv)
{
	bool ready = state != SERVREG_LOCATOR_ERR;

	if (ready)
		WRITE_ONCE(audio_pdr_state, state);
	if (ready != READ_ONCE(audio_pdr_ready)) {
		WRITE_ONCE(audio_pdr_ready, ready);
		srcu_notifier_call_chain(&audio_pdr_cb_list,
			ready ? AUDIO_PDR_FRAMEWORK_UP : AUDIO_PDR_FRAMEWORK_DOWN,
			NULL);
	}
	if (ready)
		srcu_notifier_call_chain(&audio_pdr_service_list, state, NULL);
}

int audio_pdr_register(struct notifier_block *nb)
{
	int ret;

	if (!nb)
		return -EINVAL;
	ret = srcu_notifier_chain_register(&audio_pdr_cb_list, nb);
	if (!ret && READ_ONCE(audio_pdr_ready))
		nb->notifier_call(nb, AUDIO_PDR_FRAMEWORK_UP, NULL);
	return ret;
}
EXPORT_SYMBOL(audio_pdr_register);

int audio_pdr_deregister(struct notifier_block *nb)
{
	if (!nb)
		return -EINVAL;
	return srcu_notifier_chain_unregister(&audio_pdr_cb_list, nb);
}
EXPORT_SYMBOL(audio_pdr_deregister);

void *audio_pdr_service_register(int domain_id, struct notifier_block *nb,
				int *curr_state)
{
	int ret;

	if (domain_id != AUDIO_PDR_DOMAIN_ADSP || !nb || !curr_state)
		return ERR_PTR(-EINVAL);
	if (!READ_ONCE(audio_pdr_ready))
		return ERR_PTR(-EPROBE_DEFER);
	ret = srcu_notifier_chain_register(&audio_pdr_service_list, nb);
	if (ret)
		return ERR_PTR(ret);
	*curr_state = READ_ONCE(audio_pdr_state);
	return nb;
}
EXPORT_SYMBOL(audio_pdr_service_register);

int audio_pdr_service_deregister(void *handle, struct notifier_block *nb)
{
	if (!nb || handle != nb)
		return -EINVAL;
	return srcu_notifier_chain_unregister(&audio_pdr_service_list, nb);
}
EXPORT_SYMBOL(audio_pdr_service_deregister);

static int __init audio_pdr_init(void)
{
	struct pdr_service *service;
	int ret;

	if (!IS_ENABLED(CONFIG_QCOM_PDR_HELPERS))
		return -ENODEV;
	audio_pdr = pdr_handle_alloc(audio_pdr_status, NULL);
	if (IS_ERR_OR_NULL(audio_pdr))
		return audio_pdr ? PTR_ERR(audio_pdr) : -ENODEV;
	/* Same AVS service and ADSP protection domain as the fogos firmware. */
	service = pdr_add_lookup(audio_pdr, "avs/audio", "msm/adsp/audio_pd");
	if (IS_ERR_OR_NULL(service)) {
		ret = service ? PTR_ERR(service) : -ENODEV;
		pdr_handle_release(audio_pdr);
		return ret;
	}
	return 0;
}
module_init(audio_pdr_init);

static void __exit audio_pdr_exit(void)
{
	pdr_handle_release(audio_pdr);
	srcu_cleanup_notifier_head(&audio_pdr_service_list);
	srcu_cleanup_notifier_head(&audio_pdr_cb_list);
}
module_exit(audio_pdr_exit);
MODULE_DESCRIPTION("Fogos APR audio protection-domain bridge");
MODULE_LICENSE("GPL v2");
