// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Lenovo GameZone WMI interface driver. The GameZone WMI interface provides
 * platform profile and fan curve settings for devices that fall under the
 * "Gaming Series" of Lenovo Legion devices.
 *
 * Copyright(C) 2024 Derek J. Clark <derekjohn.clark@gmail.com>
 */

#include <linux/cleanup.h>
#include <linux/dev_printk.h>
#include <linux/dmi.h>
#include <linux/notifier.h>
#include <linux/platform_profile.h>
#include <linux/types.h>
#include <linux/wmi.h>
#include "lenovo-wmi.h"

/* Interface GUIDs */
#define LENOVO_GAMEZONE_GUID "887B54E3-DDDC-4B2C-8B88-68A26A8835D0"
#define SMART_FAN_MODE_EVENT_GUID "D320289E-8FEA-41E0-86F9-611D83151B5F"

/* Method IDs */
#define WMI_METHOD_ID_SMARTFAN_SUPP 43 /* IsSupportSmartFan */
#define WMI_METHOD_ID_SMARTFAN_SET 44 /* SetSmartFanMode */
#define WMI_METHOD_ID_SMARTFAN_GET 45 /* GetSmartFanMode */

struct lenovo_gz_priv {
	struct wmi_device *wdev;
	struct device *ppdev;
	bool extreme_supported;
};

static BLOCKING_NOTIFIER_HEAD(lenovo_gz_chain_head);

struct quirk_entry {
	bool extreme_supported;
};

static struct quirk_entry quirk_no_extreme_bug = {
	.extreme_supported = false,
};

static const struct dmi_system_id fwbug_list[] = {
	{
		.ident = "Legion Go",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "Legion Go 8APU1"),
		},
		.driver_data = &quirk_no_extreme_bug,
	},
	{
		.ident = "Legion Go S",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "Legion Go S 8ARP1"),
		},
		.driver_data = &quirk_no_extreme_bug,
	},
	{},

};

/* Platform Profile Methods */
static int lenovo_gz_platform_profile_supported(struct lenovo_gz_priv *priv,
						int *supported)
{
	return lenovo_wmidev_evaluate_method_1(
		priv->wdev, 0x0, WMI_METHOD_ID_SMARTFAN_SUPP, 0, supported);
}

static int lenovo_gz_profile_get(struct device *dev,
				 enum platform_profile_option *profile)
{
	struct lenovo_gz_priv *priv = dev_get_drvdata(dev);
	int sel_prof;
	int err;

	err = lenovo_wmidev_evaluate_method_1(
		priv->wdev, 0x0, WMI_METHOD_ID_SMARTFAN_GET, 0, &sel_prof);
	if (err)
		return err;

	switch (sel_prof) {
	case SMARTFAN_MODE_QUIET:
		*profile = PLATFORM_PROFILE_QUIET;
		break;
	case SMARTFAN_MODE_BALANCED:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case SMARTFAN_MODE_PERFORMANCE:
		if (priv->extreme_supported) {
			*profile = PLATFORM_PROFILE_BALANCED_PERFORMANCE;
			break;
		}
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case SMARTFAN_MODE_EXTREME:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case SMARTFAN_MODE_CUSTOM:
		*profile = PLATFORM_PROFILE_CUSTOM;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int lenovo_gz_profile_set(struct device *dev,
				 enum platform_profile_option profile)
{
	struct lenovo_gz_priv *priv = dev_get_drvdata(dev);
	int sel_prof;
	int err;

	switch (profile) {
	case PLATFORM_PROFILE_QUIET:
		sel_prof = SMARTFAN_MODE_QUIET;
		break;
	case PLATFORM_PROFILE_BALANCED:
		sel_prof = SMARTFAN_MODE_BALANCED;
		break;
	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		sel_prof = SMARTFAN_MODE_PERFORMANCE;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		if (priv->extreme_supported) {
			sel_prof = SMARTFAN_MODE_EXTREME;
			break;
		}
		sel_prof = SMARTFAN_MODE_PERFORMANCE;
		break;
	case PLATFORM_PROFILE_CUSTOM:
		sel_prof = SMARTFAN_MODE_CUSTOM;
		break;
	default:
		return -EOPNOTSUPP;
	}

	err = lenovo_wmidev_evaluate_method_1(
		priv->wdev, 0x0, WMI_METHOD_ID_SMARTFAN_SET, sel_prof, NULL);
	if (err)
		return err;

	return 0;
}

static bool extreme_supported(int profile_support_ver)
{
	const struct dmi_system_id *dmi_id;
	struct quirk_entry *quirks;

	if (profile_support_ver < 6) {
		return false;
	}

	dmi_id = dmi_first_match(fwbug_list);
	if (!dmi_id)
		return true;

	quirks = dmi_id->driver_data;
	return quirks->extreme_supported;
}

/* Driver Setup */
static int lenovo_wmi_platform_profile_probe(void *drvdata,
					     unsigned long *choices)
{
	struct lenovo_gz_priv *priv = drvdata;
	int profile_support_ver;
	int err;

	err = lenovo_gz_platform_profile_supported(priv, &profile_support_ver);
	if (err)
		return err;

	pr_info("Profile Support Version %d\n", profile_support_ver);

	if (profile_support_ver < 1)
		return -ENODEV;

	priv->extreme_supported = extreme_supported(profile_support_ver);

	set_bit(PLATFORM_PROFILE_QUIET, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);
	set_bit(PLATFORM_PROFILE_CUSTOM, choices);

	if (priv->extreme_supported)
		set_bit(PLATFORM_PROFILE_BALANCED_PERFORMANCE, choices);
	return 0;
}

static const struct platform_profile_ops lenovo_gz_platform_profile_ops = {
	.probe = lenovo_wmi_platform_profile_probe,
	.profile_get = lenovo_gz_profile_get,
	.profile_set = lenovo_gz_profile_set,
};

static int lenovo_gz_probe(struct wmi_device *wdev, const void *context)
{
	struct lenovo_gz_priv *priv;

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->wdev = wdev;
	priv->ppdev = platform_profile_register(
		&wdev->dev, "lenovo-wmi-gamezone", priv,
		&lenovo_gz_platform_profile_ops);
	return 0;
}

int lenovo_gz_notifier_call(struct notifier_block *nb, unsigned long action,
			    void *data)
{
	if (action != SMARTFAN_MODE_EVENT)
		return NOTIFY_DONE;

	//<check *data if necessary>

	//	platform_profile_cycle(); // Cycle platform profile if necessary

	notifier_call_chain();

	return NOTIFY_OK;
}
EXPORT_SYMBOL_NS_GPL(lenovo_gz_notifier_call, GZ_WMI);

int lenovo_gz_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&lenovo_gz_chain_head, nb);
}
EXPORT_SYMBOL_NS_GPL(lenovo_gz_register_notifier, GZ_WMI);

int lenovo_gz_unregister_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&lenovo_gz_chain_head, nb);
}
EXPORT_SYMBOL_NS_GPL(lenovo_gz_unregister_notifier, GZ_WMI);

static void devm_lenovo_gz_unregister_notifier(void *data)
{
	struct notifier_block *nb = data;

	lenovo_gz_unregister_notifier(nb);
}

int devm_lenovo_gz_register_notifier(struct device *dev,
				     struct notifier_block *nb)
{
	int ret;

	ret = lenovo_gz_register_notifier(nb);
	if (ret < 0)
		return ret;

	return devm_add_action_or_reset(dev, devm_lenovo_gz_unregister_notifier,
					nb);
}
EXPORT_SYMBOL_NS_GPL(devm_lenovo_gz_register_notifier, GZ_WMI);

static void lenovo_gz_notify(struct wmi_device *wdev, union acpi_object *obj)
{
	//struct lenovo_gz_priv *priv = dev_get_drvdata(&wdev->dev);
	u32 value;
	int ret;

	if (obj->type != ACPI_TYPE_INTEGER)
		return;

	value = obj->integer.value;

	dev_dbg(&wdev->dev, "Received WMI event %u\n", value);

	ret = blocking_notifier_call_chain(&lenovo_gz_chain_head, value, NULL);
	if (ret == NOTIFY_BAD)
		return;
}

static const struct wmi_device_id lenovo_gz_id_table[] = {
	{ LENOVO_GAMEZONE_GUID, NULL },
	{ SMART_FAN_MODE_EVENT_GUID, NULL },
	{}
};

static struct wmi_driver lenovo_gz_driver = {
	.driver = { 
		.name = "lenovo_wmi_gamezone", 
		.probe_type = PROBE_PREFER_ASYNCHRONOUS, 
	},
	.id_table = lenovo_gz_id_table,
	.probe = lenovo_gz_probe,
	.notify = lenovo_gz_notify,
	.no_singleton = true,
};

module_wmi_driver(lenovo_gz_driver);

MODULE_IMPORT_NS("LENOVO_WMI");
MODULE_DEVICE_TABLE(wmi, lenovo_gz_id_table);
MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Lenovo GameZone WMI Driver");
MODULE_LICENSE("GPL");
