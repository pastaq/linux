// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * LENOVO_CAPABILITY_DATA_01 WMI data block driver. This interface provides
 * information on tunable attributes used by the "Other Method" WMI interface,
 * including if it is supported by the hardware, the default_value, max_value,
 * min_value, and step increment.
 *
 * Copyright(C) 2024 Derek J. Clark <derekjohn.clark@gmail.com>
 */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/gfp_types.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/wmi.h>
#include "lenovo-wmi.h"

/* Interface GUIDs */
#define LENOVO_CAPABILITY_DATA_01_GUID "7A8F5407-CB67-4D6E-B547-39B3BE018154"

static DEFINE_MUTEX(cd01_list_mutex);
static LIST_HEAD(cd01_wmi_list);

struct lenovo_wmi_cd01_priv {
	struct wmi_device *wdev;
	struct list_head list;
	int instance_count;
	struct capdata01 **capdata;
};

static inline struct lenovo_wmi_cd01_priv *get_first_wmi_priv(void)
{
	guard(mutex)(&cd01_list_mutex);
	return list_first_entry_or_null(&cd01_wmi_list,
					struct lenovo_wmi_cd01_priv, list);
}

struct capdata01 *lenovo_wmi_capdata01_get(u32 attribute_id)

{
	struct lenovo_wmi_cd01_priv *priv;
	int idx;

	priv = get_first_wmi_priv();
	if (!priv)
		return NULL;

	for (idx = 0; idx < priv->instance_count; idx++) {
		if (priv->capdata[idx]->id != attribute_id)
			continue;
		return priv->capdata[idx];
	}

	pr_err("Unable to find capability data for attribute_id %x\n",
	       attribute_id);
	return NULL;
}
EXPORT_SYMBOL_NS_GPL(lenovo_wmi_capdata01_get, "CAPDATA_WMI");

static int lenovo_wmi_capdata01_setup(struct lenovo_wmi_cd01_priv *priv)
{
	size_t cd_size = sizeof(struct capdata01);
	int count, idx;

	count = wmidev_instance_count(priv->wdev);

	priv->capdata = devm_kmalloc_array(&priv->wdev->dev, (size_t)count,
					   sizeof(*priv->capdata), GFP_KERNEL);
	if (!priv->capdata)
		return -ENOMEM;

	priv->instance_count = count;

	for (idx = 0; idx < count; idx++) {
		union acpi_object *ret_obj __free(kfree) = NULL;
		struct capdata01 *cap_ptr =
			devm_kmalloc(&priv->wdev->dev, cd_size, GFP_KERNEL);
		ret_obj = wmidev_block_query(priv->wdev, idx);
		if (!ret_obj)
			continue;

		if (ret_obj->type != ACPI_TYPE_BUFFER)
			continue;

		if (ret_obj->buffer.length != cd_size) {
			continue;
		}

		memcpy(cap_ptr, ret_obj->buffer.pointer,
		       ret_obj->buffer.length);
		priv->capdata[idx] = cap_ptr;
	}
	return 0;
}

static int lenovo_wmi_capdata01_probe(struct wmi_device *wdev,
				      const void *context)

{
	struct lenovo_wmi_cd01_priv *priv;

	pr_info("Probe Start.\n");
	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->wdev = wdev;
	pr_info("Allocated Priv.\n");

	dev_set_drvdata(&wdev->dev, priv);
	pr_info("Set  drvdata.\n");

	guard(mutex)(&cd01_list_mutex);
	list_add_tail(&priv->list, &cd01_wmi_list);
	pr_info("Added to list.\n");

	return lenovo_wmi_capdata01_setup(priv);
}

static void lenovo_wmi_capdata01_remove(struct wmi_device *wdev)
{
	struct lenovo_wmi_cd01_priv *priv = dev_get_drvdata(&wdev->dev);

	guard(mutex)(&cd01_list_mutex);
	list_del(&priv->list);
}

static const struct wmi_device_id lenovo_wmi_capdata01_id_table[] = {
	{ LENOVO_CAPABILITY_DATA_01_GUID, NULL },
	{}
};

static struct wmi_driver lenovo_wmi_capdata01_driver = {
	.driver = { .name = "lenovo_wmi_capdata01" },
	.id_table = lenovo_wmi_capdata01_id_table,
	.probe = lenovo_wmi_capdata01_probe,
	.remove = lenovo_wmi_capdata01_remove,
};

module_wmi_driver(lenovo_wmi_capdata01_driver);

MODULE_DEVICE_TABLE(wmi, lenovo_wmi_capdata01_id_table);
MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Lenovo Capability Data 01 WMI Driver");
MODULE_LICENSE("GPL");
