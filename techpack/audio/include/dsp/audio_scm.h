/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _FOGOS_AUDIO_SCM_H
#define _FOGOS_AUDIO_SCM_H
#include <linux/bitops.h>
#include <linux/qcom_scm.h>
#include <linux/slab.h>

/* Translate the retained calibration ownership lists to the 6.1 SCM API. */
static inline int audio_hyp_assign_phys(phys_addr_t address, size_t size,
		const int *sources, int source_count, const int *destinations,
		const int *permissions, int destination_count)
{
	struct qcom_scm_vmperm *perms;
	u64 source_mask = 0;
	int i, ret;

	if (!sources || !destinations || !permissions || source_count <= 0 ||
	    destination_count <= 0 || !size)
		return -EINVAL;
	for (i = 0; i < source_count; i++) {
		if (sources[i] < 0 || sources[i] >= 64)
			return -EINVAL;
		source_mask |= BIT_ULL(sources[i]);
	}
	perms = kcalloc(destination_count, sizeof(*perms), GFP_KERNEL);
	if (!perms)
		return -ENOMEM;
	for (i = 0; i < destination_count; i++) {
		if (destinations[i] < 0 || destinations[i] >= 64) {
			kfree(perms);
			return -EINVAL;
		}
		perms[i].vmid = destinations[i];
		perms[i].perm = permissions[i];
	}
	ret = qcom_scm_assign_mem(address, size, &source_mask, perms,
				destination_count);
	kfree(perms);
	return ret;
}
#endif
