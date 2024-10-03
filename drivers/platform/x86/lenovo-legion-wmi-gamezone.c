// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Lenovo GameZone WMI interface driver. The GameZone WMI interface provides
 * platform profile and fan curve settings for devices that fall under the
 * "Gaming Series" of Lenovo Legion devices.
 *
 * Copyright(C) 2024 Derek J. Clark <derekjohn.clark@gmail.com>
 *
 */

#include <linux/acpi.h>
#include <linux/debugfs.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/wmi.h>

#define LENOVO_GAMEZONE_GUID     "887B54E3-DDDC-4B2C-8B88-68A26A8835D0"

/* Platform Profile */
#define WMI_METHOD_ID_SMARTFAN_SUPP 43   /* IsSupportSmartFan */
#define WMI_METHOD_ID_SMARTFAN_SET  44   /* SetSmartFanMode */
#define WMI_METHOD_ID_SMARTFAN_GET  45   /* GetSmartFanMode */
#define SMARTFAN_MODE_QUIET         0x01
#define SMARTFAN_MODE_BALANCED      0x02
#define SMARTFAN_MODE_PERFORMANCE   0x03
#define SMARTFAN_MODE_CUSTOM        0xFF /* Enables power limit setting */

static const struct wmi_device_id gamezone_wmi_id_table[] = {
        { LENOVO_GAMEZONE_GUID, NULL }, /* LENOVO_GAMEZONE_DATA */
        { }
};

MODULE_DEVICE_TABLE(wmi, gamezone_wmi_id_table);

struct gamezone_wmi {
        enum platform_profile_option current_profile;
        struct platform_profile_handler pprof;
        bool platform_profile_support;
	struct wmi_device *wdev;
};

/* Platform Profile Methods */
static int gamezone_wmi_platform_profile_supported(struct platform_profile_handler *pprof)
{
	struct gamezone_wmi *gz_wmi;
	struct acpi_buffer input = { 0, NULL };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status;
	union acpi_object *result = NULL;
	int supported;

	gz_wmi = container_of(pprof, struct gamezone_wmi, pprof);
	
	status = wmidev_evaluate_method(gz_wmi->wdev, 0x0, WMI_METHOD_ID_SMARTFAN_SUPP, &input, &output);
	
	if (ACPI_FAILURE(status)) {
		printk("Failed to read smartfan supported.\n");
		return -EIO;
	}

	result = output.pointer;
	if (result && result->type == ACPI_TYPE_INTEGER) {
		supported = result->integer.value;
	} else {
		printk("Failed to read smartfan supported.\n");
		kfree(output.pointer);
		return -AE_ERROR;
	}

	kfree(output.pointer);
	printk("Got result for platform profile supported: %u\n", supported);
	return supported;

}

static int gamezone_wmi_platform_profile_get(struct platform_profile_handler *pprof,
					enum platform_profile_option *profile)
{
	struct gamezone_wmi *gz_wmi;
	struct acpi_buffer input = { 0, NULL };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status;
	union acpi_object *result = NULL;
	unsigned int sel_prof;


	gz_wmi = container_of(pprof, struct gamezone_wmi, pprof);
	
	/* This is deprecated, use wmidev_evaluate_method */
	status = wmidev_evaluate_method(gz_wmi->wdev, 0x0, WMI_METHOD_ID_SMARTFAN_GET, &input, &output);
	
	if (ACPI_FAILURE(status)) {
		printk("Failed to read current platform profile\n");
		return -EIO;
	}

	result = output.pointer;

	if (result && result->type == ACPI_TYPE_INTEGER) {
		sel_prof = result->integer.value;
		printk("Got profile: %u\n", sel_prof);
	} else {
		printk("Failed to read current platform profile\n");
		kfree(output.pointer);
		return -AE_ERROR;
	}

	switch (sel_prof) {
	case SMARTFAN_MODE_QUIET:
		printk("Got platform profile: QUIET\n");
		*profile = PLATFORM_PROFILE_QUIET;
		break;
	case SMARTFAN_MODE_BALANCED:
		printk("Got platform profile: BALANCED\n");
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case SMARTFAN_MODE_PERFORMANCE:
		printk("Got platform profile: PEROFRMANCE\n");
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	case SMARTFAN_MODE_CUSTOM:
		printk("Got platform profile: CUSTOM\n");
		kfree(output.pointer);
		return -EINVAL;

		/* Requires new CUSTOM mode in platform_profile */
		/* *profile = PLATFORM_PROFILE_CUSTOM;
		break; */
	default:
		printk("Got invald platform profile, %u\n", sel_prof);
		kfree(output.pointer);
		return -EINVAL;
	}

	/* Free the memory allocated for output */
	kfree(output.pointer);

	return 0;
}

static int gamezone_wmi_platform_profile_set(struct platform_profile_handler *pprof,
					enum platform_profile_option profile)
{
	struct gamezone_wmi *gz_wmi;
	struct acpi_buffer input; 
	acpi_status status;
	int sel_prof;

	gz_wmi = container_of(pprof, struct gamezone_wmi, pprof);
	input.length = sizeof(u8);

	switch (profile) {
	case PLATFORM_PROFILE_QUIET:
		sel_prof = SMARTFAN_MODE_QUIET;
		break;
	case PLATFORM_PROFILE_BALANCED:
		sel_prof = SMARTFAN_MODE_BALANCED;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		sel_prof = SMARTFAN_MODE_PERFORMANCE;
		break;
	/* Requires new CUSTOM mode in platform_profile */
	/* case PLATFORM_PROFILE_CUSTOM:
		sel_prof = SMARTFAN_MODE_CUSTOM;
		break; */
	default:
		kfree(input.pointer);
		return -EOPNOTSUPP;
	}

	input.pointer = &sel_prof;

	status = wmidev_evaluate_method(gz_wmi->wdev, 0x0, WMI_METHOD_ID_SMARTFAN_SET, &input, NULL);

	if (ACPI_FAILURE(status)) {
		printk("Failed to set current platform profile\n");
		return -EIO;
	}

	return 0;
}

static int platform_profile_setup(struct gamezone_wmi *gz_wmi)
{
	int err;

	gz_wmi->pprof.profile_get = gamezone_wmi_platform_profile_get;
	gz_wmi->pprof.profile_set = gamezone_wmi_platform_profile_set;

	gz_wmi->platform_profile_support = gamezone_wmi_platform_profile_supported(&gz_wmi->pprof);
	if (!gz_wmi->platform_profile_support) {
		printk("Platform profiles are not supported by this device.\n");
		return -ENOTSUPP;
	}

	err = gamezone_wmi_platform_profile_get(&gz_wmi->pprof, &gz_wmi->current_profile);
	if (err) {
		printk("Failed to get current platform profile: %d\n", err);
		return err;
	}

	/* Setup supported modes */
	set_bit(PLATFORM_PROFILE_QUIET, gz_wmi->pprof.choices);
	set_bit(PLATFORM_PROFILE_BALANCED, gz_wmi->pprof.choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, gz_wmi->pprof.choices);
	/* set_bit(PLATFORM_PROFILE_CUSTOM, gz_wmi->pprof.choices); */

	/* Create platform_profile structure and register */
	err = platform_profile_register(&gz_wmi->pprof);
	if (err) {
		printk("Failed to register platform profile support: %d\n", err);
		return err;
	}

	printk("Registered platform profile support\n");
	return 0;
}

/* Driver Setup */
static int gamezone_wmi_probe(struct wmi_device *wdev, const void *context)
{
	printk("Lenovo GameZone WMI probe\n");
	struct gamezone_wmi *gz_wmi;
	int err;

	gz_wmi = kzalloc(sizeof(struct gamezone_wmi), GFP_KERNEL);
	if (!gz_wmi)
		return -ENOMEM;

	gz_wmi->wdev = wdev;
	err = platform_profile_setup(gz_wmi);
	if (err) {
		kfree(gz_wmi);
	}
	return err;
}

static void gamezone_wmi_remove(struct wmi_device *wdev)
{
	int err;
	err = platform_profile_remove();
	if (err) {
		printk("Failed to remove platform profile: %d\n", err);
	} else {
		printk("Removed platform profile support\n");
	}

	return;
}

static void gamezone_wmi_notify(struct wmi_device *device, union acpi_object *data)
{
	printk("Lenovo GameZone WMI notify\n");
    	return;
}

static struct wmi_driver gamezone_wmi_driver = {
       .driver = {
       		.name = "gamezone_wmi",
       },
       .id_table = gamezone_wmi_id_table,
       .probe = gamezone_wmi_probe,
       .remove = gamezone_wmi_remove,
       .notify = gamezone_wmi_notify,
       /* .no_notify_data = true,
       .no_singleton = true, */
};

module_wmi_driver(gamezone_wmi_driver);

MODULE_DEVICE_TABLE(wmi, gamezone_wmi_id_table);
MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Lenovo GameZone WMI Driver");
MODULE_LICENSE("GPL");
