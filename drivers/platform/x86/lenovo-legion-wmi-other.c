// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Lenovo Legion other method driver. This driver uses the fw_attributes
 * class to expose the various WMI functions provided by the "Other Method" WMI
 * interface for CPU and GPU power limit for devices that fall under the
 * "Gaming Series" of Lenovo Legion devices. These typically don't fit anywhere
 * else in the sysfs and are set in Windows using one of Lenovo's multiple user
 * applications.
 *
 * Copyright(C) 2024 Derek J. Clark <derekjohn.clark@gmail.com>
 *
 */

#include <linux/acpi.h>
#include <linux/mutex.h>
#include <linux/wmi.h>
#include "firmware_attributes_class.h"

#define LENOVO_OTHER_METHOD_GUID "DC2A8805-3A8C-41BA-A6F7-092E0089CD3B"

/* Power Limit */
#define WMI_METHOD_ID_SPPT       0x0101  /* Short Term Power Limit */
#define WMI_METHOD_ID_FPPT       0x0102  /* Long Term Power Limit */
#define WMI_METHOD_ID_SPL        0x0103  /* Peak Power Limit */
//#define WMI_METHOD_ID_CPU_TEMP   0x0104  /* CPU Temp Limit */
//#define WMI_METHOD_ID_APU_SPPT   0x0105  /* APU Slow Power Limit */ /* Intel? */
//#define WMI_METHOD_ID_CPU_PL     0x0106  /* Cross Loading CPU Power Limit */ /* Intel? */

static const struct wmi_device_id other_method_wmi_id_table[] = {
        { LENOVO_OTHER_METHOD_GUID, NULL },
        { }
};

/* Tunable Attributes */
struct tunables {

	u32 ppt_pl2_sppt; // cpu
	u32 ppt_pl2_sppt_max; // cpu
	u32 ppt_pl2_sppt_min; // cpu
	u32 ppt_fppt; // cpu
	u32 ppt_fppt_max; // cpu
	u32 ppt_fppt_min; // cpu
	u32 ppt_pl1_spl; // cpu
	u32 ppt_pl1_spl_max; // cpu
	u32 ppt_pl1_spl_min; // cpu
};

static const struct class *fw_attr_class;

struct other_method_priv {
	struct wmi_device *wdev;
	struct device *fw_attr_dev;
	struct kset *fw_attr_kset;

	struct tunables *tunables;

	struct mutex mutex;
};

static struct other_method_priv other_method = { .mutex = __MUTEX_INITIALIZER(
							 other_method.mutex) };

struct fw_attrs_group {
	bool pending_reboot;
};

static struct fw_attrs_group fw_attrs = {
	.pending_reboot = false,
};

struct other_method_attr_group {
	const struct attribute_group *attr_group;
};

struct other_method_command {
        uint32_t method_id : 16;
        uint32_t mode      : 8;
        uint32_t tail      : 8;
};

static int other_method_attribute_get(struct wmi_device *wdev, u16 method_id)
{
	// Get the current platform_profile
	u8 mode = 03;
	struct other_method_command method_arg = { method_id, mode, 0x00 };
	struct acpi_buffer input = { (acpi_size) sizeof(method_arg), &method_arg };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	return 0;
}

static int other_method_attribute_set(struct wmi_device *wdev, u16 method_id,
								u8 value)
{

	return 0;

}

//static const struct other_method_attr_group other_method_attr_groups[] = {
//	{ &ppt_pl2_sppt_attr_group, WMI_METHOD_ID_SPPT},
//	{ &ppt_fppt_attr_group, WMI_METHOD_ID_FPPT},
//	{ &ppt_pl1_spl_attr_group, WMI_METHOD_ID_SPL},
//};


/* Driver Setup */
static int other_method_wmi_probe(struct wmi_device *wdev, const void *context)
{
        printk("Lenovo Other Method WMI probe\n");
	//int err;

	other_method.wdev = wdev;
	other_method.tunables = kzalloc(sizeof(struct tunables), GFP_KERNEL);
	if (!other_method.tunables)
		return -ENOMEM;

	//init_tunables(other_method.tunables);

	//err = other_fw_attr_add();
	//if (err)
	//	return err;

	return 0;
       }

static void other_method_wmi_remove(struct wmi_device *wdev)
{
        printk("Lenovo Other Method WMI remove\n");
        return;
}

static void other_method_wmi_notify(struct wmi_device *device, union acpi_object *data)
{
        printk("Lenovo Other Method WMI notify\n");
	mutex_lock(&other_method.mutex);

	//sysfs_remove_file(&other_method.fw_attr_kset->kobj, &pending_reboot.attr);
	kset_unregister(other_method.fw_attr_kset);
	device_destroy(fw_attr_class, MKDEV(0, 0));
	fw_attributes_class_put();

	mutex_unlock(&other_method.mutex);

        return;
}

static struct wmi_driver other_method_wmi_driver = {
       .driver = {
                .name = "other_method_wmi",
       },
       .id_table = other_method_wmi_id_table,
       .probe = other_method_wmi_probe,
       .remove = other_method_wmi_remove,
       .notify = other_method_wmi_notify,
       /* .no_notify_data = true,
       .no_singleton = true, */
};

module_wmi_driver(other_method_wmi_driver);

MODULE_DEVICE_TABLE(wmi, other_method_wmi_id_table);
MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Lenovo Legion Other Method Driver");
MODULE_LICENSE("GPL");
