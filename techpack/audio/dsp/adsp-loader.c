// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2014, 2017-2021, The Linux Foundation. All rights reserved.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <ipc/apr.h>
#include <linux/of_device.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/nvmem-consumer.h>
#include <linux/slab.h>
#include <soc/qcom/boot_stats.h>
#include <dsp/audio_remoteproc.h>
#include <linux/mutex.h>

#define Q6_PIL_GET_DELAY_MS 100
#define BOOT_CMD 1
#define SSR_RESET_CMD 1
#define IMAGE_UNLOAD_CMD 0
#define MAX_FW_IMAGES 4
#define BOOT_FOR_EARLY_CHIME_CMD 2

static ssize_t adsp_boot_store(struct kobject *kobj,
	struct kobj_attribute *attr,
	const char *buf, size_t count);

static ssize_t adsp_ssr_store(struct kobject *kobj,
	struct kobj_attribute *attr,
	const char *buf, size_t count);

struct adsp_loader_private {
	void *pil_h;
	struct kobject *boot_adsp_obj;
	struct attribute_group *attr_group;
	char *adsp_fw_name;
};

static struct kobj_attribute adsp_boot_attribute =
	__ATTR(boot, 0220, NULL, adsp_boot_store);

static struct kobj_attribute adsp_ssr_attribute =
	__ATTR(ssr, 0220, NULL, adsp_ssr_store);

static struct attribute *attrs[] = {
	&adsp_boot_attribute.attr,
	&adsp_ssr_attribute.attr,
	NULL,
};

static DEFINE_MUTEX(adsp_loader_lock);
static struct work_struct adsp_ldr_work;
static struct platform_device *adsp_private;
static void adsp_loader_unload(struct platform_device *pdev);

static int adsp_restart_subsys(void)
{
	struct adsp_loader_private *priv;
	int ret = -ENODEV;

	mutex_lock(&adsp_loader_lock);
	if (!adsp_private)
		goto out;
	priv = platform_get_drvdata(adsp_private);
	if (priv && !IS_ERR_OR_NULL(priv->pil_h)) {
		/* Remoteproc owns crash recovery and the restart notifications. */
		rproc_report_crash(priv->pil_h, RPROC_FATAL_ERROR);
		ret = 0;
	}
out:
	mutex_unlock(&adsp_loader_lock);
	return ret;
}

static void adsp_load_state_notify_cb(enum apr_subsys_state state,
						void *phandle)
{
	struct platform_device *pdev = adsp_private;
	struct adsp_loader_private *priv = NULL;

	priv = platform_get_drvdata(pdev);
	if (!priv)
		return;
	if (phandle != adsp_private) {
		pr_err("%s:callback is not for adsp-loader client\n", __func__);
		return;
	}
	pr_debug("%s:: Received cb for ADSP restart\n", __func__);
	if (state == APR_SUBSYS_UNKNOWN)
		adsp_restart_subsys();
	else
		pr_debug("%s:Ignore restart request for ADSP", __func__);
}

static void adsp_load_fw(struct work_struct *work)
{
	struct platform_device *pdev;
	struct adsp_loader_private *priv;
	struct rproc *rproc;
	const char *image;
	bool modem = false;

	mutex_lock(&adsp_loader_lock);
	pdev = adsp_private;
	if (!pdev)
		goto out;
	priv = platform_get_drvdata(pdev);
	if (!priv || priv->pil_h)
		goto out;
	if (!of_property_read_string(pdev->dev.of_node,
			"qcom,proc-img-to-load", &image))
		modem = !strcmp(image, "modem");
	rproc = audio_rproc_get_boot(&pdev->dev,
		modem ? "qcom,modem-rproc-handle" : "qcom,rproc-handle",
		modem ? NULL : priv->adsp_fw_name);
	if (IS_ERR(rproc)) {
		dev_err(&pdev->dev, "Failed to boot %s: %ld\n",
			modem ? "modem" : "ADSP", PTR_ERR(rproc));
		goto out;
	}
	priv->pil_h = rproc;
	if (modem)
		apr_set_modem_state(APR_SUBSYS_LOADED);
	else
		apr_register_adsp_state_cb(adsp_load_state_notify_cb, pdev);
out:
	mutex_unlock(&adsp_loader_lock);
}

static void adsp_loader_do(struct platform_device *pdev)
{
	schedule_work(&adsp_ldr_work);
}

static ssize_t adsp_ssr_store(struct kobject *kobj,
	struct kobj_attribute *attr,
	const char *buf,
	size_t count)
{
	int ssr_command = 0;
	struct platform_device *pdev = adsp_private;
	struct adsp_loader_private *priv = NULL;
	int rc = -EINVAL;

	dev_dbg(&pdev->dev, "%s: going to call adsp ssr\n ", __func__);

	priv = platform_get_drvdata(pdev);
	if (!priv)
		return rc;

	if (kstrtoint(buf, 10, &ssr_command) < 0)
		return -EINVAL;

	if (ssr_command != SSR_RESET_CMD)
		return -EINVAL;

	rc = adsp_restart_subsys();
	if (rc)
		return rc;
	dev_dbg(&pdev->dev, "%s :: ADSP restarted\n", __func__);
	return count;
}

static ssize_t adsp_boot_store(struct kobject *kobj,
	struct kobj_attribute *attr,
	const char *buf,
	size_t count)
{
	int boot = 0;

	if (sscanf(buf, "%du", &boot) != 1) {
		pr_err("%s: failed to read boot info from string\n", __func__);
		return -EINVAL;
	}

	if (boot == BOOT_CMD) {
		pr_debug("%s: going to call adsp_loader_do\n", __func__);
		adsp_loader_do(adsp_private);
	} else if (boot == IMAGE_UNLOAD_CMD) {
		pr_debug("%s: going to call adsp_unloader\n", __func__);
		adsp_loader_unload(adsp_private);
	} else if (boot == BOOT_FOR_EARLY_CHIME_CMD) {
		pr_debug("%s: going to call adsp_load_fw\n", __func__);
		adsp_load_fw(NULL);
	}

	return count;
}

static void adsp_loader_unload(struct platform_device *pdev)
{
	struct adsp_loader_private *priv;

	cancel_work_sync(&adsp_ldr_work);
	mutex_lock(&adsp_loader_lock);
	priv = platform_get_drvdata(pdev);
	if (priv) {
		audio_rproc_shutdown_put(priv->pil_h);
		priv->pil_h = NULL;
	}
	mutex_unlock(&adsp_loader_lock);
}

static int adsp_loader_init_sysfs(struct platform_device *pdev)
{
	int ret = -EINVAL;
	struct adsp_loader_private *priv = NULL;

	adsp_private = NULL;
	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		return ret;
	}

	platform_set_drvdata(pdev, priv);

	priv->pil_h = NULL;
	priv->boot_adsp_obj = NULL;
	priv->attr_group = devm_kzalloc(&pdev->dev,
				sizeof(*(priv->attr_group)),
				GFP_KERNEL);
	if (!priv->attr_group) {
		ret = -ENOMEM;
		goto error_return;
	}

	priv->attr_group->attrs = attrs;

	priv->boot_adsp_obj = kobject_create_and_add("boot_adsp", kernel_kobj);
	if (!priv->boot_adsp_obj) {
		dev_err(&pdev->dev, "%s: sysfs create and add failed\n",
						__func__);
		ret = -ENOMEM;
		goto error_return;
	}

	ret = sysfs_create_group(priv->boot_adsp_obj, priv->attr_group);
	if (ret) {
		dev_err(&pdev->dev, "%s: sysfs create group failed %d\n",
							__func__, ret);
		goto error_return;
	}

	adsp_private = pdev;

	return 0;

error_return:

	if (priv->boot_adsp_obj) {
		kobject_del(priv->boot_adsp_obj);
		priv->boot_adsp_obj = NULL;
	}

	return ret;
}

static int adsp_loader_remove(struct platform_device *pdev)
{
	struct adsp_loader_private *priv = NULL;

	priv = platform_get_drvdata(pdev);

	if (!priv)
		return 0;

	adsp_loader_unload(pdev);

	if (priv->boot_adsp_obj) {
		sysfs_remove_group(priv->boot_adsp_obj, priv->attr_group);
		kobject_del(priv->boot_adsp_obj);
		priv->boot_adsp_obj = NULL;
	}

	return 0;
}

static int adsp_loader_probe(struct platform_device *pdev)
{
	struct adsp_loader_private *priv = NULL;
	struct nvmem_cell *cell;
	size_t len;
	u32 *buf;
	const char **adsp_fw_name_array = NULL;
	int adsp_fw_cnt;
	u32* adsp_fw_bit_values = NULL;
	int i;
	int fw_name_size;
	u32 adsp_var_idx = 0;
	int ret = 0;
	u32 adsp_fuse_not_supported = 0;
	const char *adsp_fw_name;

	ret = adsp_loader_init_sysfs(pdev);
	if (ret != 0) {
		dev_err(&pdev->dev, "%s: Error in initing sysfs\n", __func__);
		return ret;
	}

	priv = platform_get_drvdata(pdev);
	/* get adsp variant idx */
	cell = nvmem_cell_get(&pdev->dev, "adsp_variant");
	if (IS_ERR_OR_NULL(cell)) {
		dev_dbg(&pdev->dev, "%s: FAILED to get nvmem cell \n",
			__func__);

		/*
		 * When ADSP variant read from fuse register is not
		 * supported, check if image with different fw image
		 * name needs to be loaded
		 */
		ret = of_property_read_u32(pdev->dev.of_node,
					  "adsp-fuse-not-supported",
					  &adsp_fuse_not_supported);
		if (ret) {
			dev_dbg(&pdev->dev,
				"%s: adsp_fuse_not_supported prop not found %d\n",
				__func__, ret);
			goto wqueue;
		}

		if (adsp_fuse_not_supported) {
			/* Read ADSP firmware image name */
			ret = of_property_read_string(pdev->dev.of_node,
						"adsp-fw-name",
						 &adsp_fw_name);
			if (ret < 0) {
				dev_dbg(&pdev->dev, "%s: unable to read fw-name\n",
					__func__);
				goto wqueue;
			}

			fw_name_size = strlen(adsp_fw_name) + 1;
			priv->adsp_fw_name = devm_kzalloc(&pdev->dev,
						fw_name_size,
						GFP_KERNEL);
			if (!priv->adsp_fw_name)
				goto wqueue;
			strlcpy(priv->adsp_fw_name, adsp_fw_name,
				fw_name_size);
		}
		goto wqueue;
	}
	buf = nvmem_cell_read(cell, &len);
	nvmem_cell_put(cell);
	if (IS_ERR_OR_NULL(buf)) {
		dev_dbg(&pdev->dev, "%s: FAILED to read nvmem cell \n", __func__);
		goto wqueue;
	}
	if (len <= 0 || len > sizeof(u32)) {
		dev_dbg(&pdev->dev, "%s: nvmem cell length out of range: %zu\n",
			__func__, len);
		kfree(buf);
		goto wqueue;
	}
	memcpy(&adsp_var_idx, buf, len);
	kfree(buf);

	/* Get count of fw images */
	adsp_fw_cnt = of_property_count_strings(pdev->dev.of_node,
						"adsp-fw-names");
	if (adsp_fw_cnt <= 0 || adsp_fw_cnt > MAX_FW_IMAGES) {
		dev_dbg(&pdev->dev, "%s: Invalid number of fw images %d",
			__func__, adsp_fw_cnt);
		goto wqueue;
	}

	adsp_fw_bit_values = devm_kzalloc(&pdev->dev,
				adsp_fw_cnt * sizeof(u32), GFP_KERNEL);
	if (!adsp_fw_bit_values)
		goto wqueue;

	/* Read bit values corresponding to each firmware image entry */
	ret = of_property_read_u32_array(pdev->dev.of_node,
					 "adsp-fw-bit-values",
					 adsp_fw_bit_values,
					 adsp_fw_cnt);
	if (ret) {
		dev_dbg(&pdev->dev, "%s: unable to read fw-bit-values\n",
			__func__);
		goto wqueue;
	}

	adsp_fw_name_array = devm_kzalloc(&pdev->dev,
				adsp_fw_cnt * sizeof(char *), GFP_KERNEL);
	if (!adsp_fw_name_array)
		goto wqueue;

	/* Read ADSP firmware image names */
	ret = of_property_read_string_array(pdev->dev.of_node,
					 "adsp-fw-names",
					 adsp_fw_name_array,
					 adsp_fw_cnt);
	if (ret < 0) {
		dev_dbg(&pdev->dev, "%s: unable to read fw-names\n",
			__func__);
		goto wqueue;
	}

	for (i = 0; i < adsp_fw_cnt; i++) {
		if (adsp_fw_bit_values[i] == adsp_var_idx) {
			fw_name_size = strlen(adsp_fw_name_array[i]) + 1;
			priv->adsp_fw_name = devm_kzalloc(&pdev->dev,
						fw_name_size,
						GFP_KERNEL);
			if (!priv->adsp_fw_name)
				goto wqueue;
			strlcpy(priv->adsp_fw_name, adsp_fw_name_array[i],
				fw_name_size);
			break;
		}
	}
wqueue:
	INIT_WORK(&adsp_ldr_work, adsp_load_fw);
	if (adsp_fw_bit_values)
		devm_kfree(&pdev->dev, adsp_fw_bit_values);
	if (adsp_fw_name_array)
		devm_kfree(&pdev->dev, adsp_fw_name_array);
	return 0;

}

static const struct of_device_id adsp_loader_dt_match[] = {
	{ .compatible = "qcom,adsp-loader" },
	{ }
};
MODULE_DEVICE_TABLE(of, adsp_loader_dt_match);

static struct platform_driver adsp_loader_driver = {
	.driver = {
		.name = "adsp-loader",
		.owner = THIS_MODULE,
		.of_match_table = adsp_loader_dt_match,
		.suppress_bind_attrs = true,
	},
	.probe = adsp_loader_probe,
	.remove = adsp_loader_remove,
};

static int __init adsp_loader_init(void)
{
	return platform_driver_register(&adsp_loader_driver);
}
module_init(adsp_loader_init);

static void __exit adsp_loader_exit(void)
{
	platform_driver_unregister(&adsp_loader_driver);
}
module_exit(adsp_loader_exit);

MODULE_DESCRIPTION("ADSP Loader module");
MODULE_LICENSE("GPL v2");
