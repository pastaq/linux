// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Lenovo Other Method WMI interface driver. This driver uses the fw_attributes
 * class to expose the various WMI functions provided by the "Other Method" WMI
 * interface. This enables CPU and GPU power limit as well as various other
 * attributes for devices that fall under the "Gaming Series" of Lenovo laptop
 * devices. Each attribute exposed by the "Other Method"" interface has a
 * corresponding LENOVO_CAPABILITY_DATA_01 struct that allows the driver to
 * probe details about the attribute such as set/get support, step, min, max,
 * and default value. Each attibute has multiple pages, one for each of the
 * fan profiles managed by the GameZone interface, so it must be probed prior
 * to returning the current_value.
 *
 * These attributes typically don't fit anywhere else in the sysfs and are set
 * in Windows using one of Lenovo's multiple user applications.
 *
 * Copyright(C) 2024 Derek J. Clark <derekjohn.clark@gmail.com>
 */

//#include <linux/kdev_t.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/wmi.h>
#include "lenovo-wmi.h"
#include "firmware_attributes_class.h"

/* Interface GUIDs */
#define LENOVO_OTHER_METHOD_GUID "DC2A8805-3A8C-41BA-A6F7-092E0089CD3B"

/* Device IDs */
#define WMI_DEVICE_ID_CPU 0x01

/* WMI_DEVICE_ID_CPU feature IDs */
#define WMI_FEATURE_ID_CPU_SPPT 0x01 /* Short Term Power Limit */
#define WMI_FEATURE_ID_CPU_FPPT 0x03 /* Long Term Power Limit */
#define WMI_FEATURE_ID_CPU_SPL 0x02 /* Peak Power Limit */

/* Type IDs*/
#define WMI_TYPE_ID_NONE 0x00

/* Method IDs */
#define WMI_FAN_TABLE_GET 5 /* Other Mode FAN_METHOD Getter */
#define WMI_FAN_TABLE_SET 6 /* Other Mode FAN_METHOD Setter */
#define WMI_FEATURE_VALUE_GET 17 /* Other Mode Getter */
#define WMI_FEATURE_VALUE_SET 18 /* Other Mode Setter */

/* Attribute ID bitmasks */
#define ATTR_DEV_ID_MASK GENMASK(31, 24)
#define ATTR_FEAT_ID_MASK GENMASK(23, 16)
#define ATTR_MODE_ID_MASK GENMASK(15, 8)
#define ATTR_TYPE_ID_MASK GENMASK(7, 0)

static DEFINE_MUTEX(om_list_mutex);
static LIST_HEAD(om_wmi_list);

struct lenovo_wmi_om_priv {
	struct wmi_device *wdev;
	struct device *fw_attr_dev;
	struct kset *fw_attr_kset;
	struct list_head list;
};

static inline struct lenovo_wmi_om_priv *get_first_wmi_priv(void)
{
	guard(mutex)(&om_list_mutex);
	return list_first_entry_or_null(&om_wmi_list, struct lenovo_wmi_om_priv,
					list);
}

enum attribute_property {
	DEFAULT_VAL,
	MAX_VAL,
	MIN_VAL,
	STEP_VAL,
	SUPPORTED,
};

/* Tunable attribute that uses LENOVO_CAPABILITY_DATA_01 */
struct tunable_attr_01 {
	struct capdata01 *capdata;
	u32 type_id;
	u32 device_id;
	u32 feature_id;
	u32 store_value;
};

/* Fan Table Data */
struct get_fan_table_data {
	u32 fan_length;
	u32 fan_data; /* fan_data[fan_length] */
	u32 sensor_length;
	u32 sensor_data; /* sensor_data[sensor_length] */
};

struct set_fan_table_data {
	u8 mode;
	u8 fan_id;
	u32 fan_length;
	u16 fan_speed; /* fan_speed[fan_length] */
	u8 sensor_id;
	u32 sensor_length;
	u16 sensor_temp; /* sensor_temp[sensor_length]*/
};

/* Tunable Attributes */
struct tunable_attr_01 ppt_pl1_spl = { .device_id = WMI_DEVICE_ID_CPU,
				       .feature_id = WMI_FEATURE_ID_CPU_SPL,
				       .type_id = WMI_TYPE_ID_NONE };
struct tunable_attr_01 ppt_pl2_sppt = { .device_id = WMI_DEVICE_ID_CPU,
					.feature_id = WMI_FEATURE_ID_CPU_SPPT,
					.type_id = WMI_TYPE_ID_NONE };
struct tunable_attr_01 ppt_pl3_fppt = { .device_id = WMI_DEVICE_ID_CPU,
					.feature_id = WMI_FEATURE_ID_CPU_FPPT,
					.type_id = WMI_TYPE_ID_NONE };

struct capdata01_attr_group {
	const struct attribute_group *attr_group;
	struct tunable_attr_01 *tunable_attr;
};

static const struct class *fw_attr_class;

#define FW_ATTR_FOLDER "lenovo-wmi-other"

static ssize_t int_type_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	return sysfs_emit(buf, "integer\n");
}

/**
 * attr_capdata01_show() - Get the value of the specified attribute property
 * from LENOVO_CAPABILITY_DATA_01.
 * @kobj: Pointer to the driver object.
 * @kobj_attribute: Pointer to the attribute calling this function.
 * @buf: The buffer to write to.
 * @tunable_attr: The attribute to be read.
 * @prop: The property of this attribute to be read.
 *
 * This function is intended to be generic so it can be called from any "_show"
 * attribute which works only with integers.
 *
 * If the WMI is success, then the sysfs attribute is notified.
 *
 * Returns: Either count, or an error.
 */
static ssize_t attr_capdata01_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf,
				   struct tunable_attr_01 *tunable_attr,
				   enum attribute_property prop)
{
	int retval;

	switch (prop) {
	case DEFAULT_VAL:
		retval = tunable_attr->capdata->default_value;
		break;
	case MAX_VAL:
		retval = tunable_attr->capdata->max_value;
		break;
	case MIN_VAL:
		retval = tunable_attr->capdata->min_value;
		break;
	case STEP_VAL:
		retval = tunable_attr->capdata->step;
		break;
	default:
		return -EINVAL;
	}
	return sysfs_emit(buf, "%d\n", retval);
}

/* Simple attribute creation */

/*
 * att_current_value_store() - Set the current value of the given attribute
 * @kobj: Pointer to the driver object.
 * @kobj_attribute: Pointer to the attribute calling this function.
 * @buf: The buffer to read from, this is parsed to `int` type.
 * @count: Required by sysfs attribute macros, pass in from the callee attr.
 * @tunable_attr: The attribute to be stored.
 *
 * This function is intended to be generic so it can be called from any
 * attribute's "current_value_store" which works only with integers. The
 * integer to be sent to the WMI method is range checked and an error returned
 * if out of range.
 *
 * If the value is valid and WMI is success, then the sysfs attribute is
 * notified.
 *
 * Returns: Either count, or an error.
 */
static ssize_t attr_current_value_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count,
					struct tunable_attr_01 *tunable_attr)
{
	u32 attribute_id =
		FIELD_PREP(ATTR_DEV_ID_MASK, tunable_attr->device_id) |
		FIELD_PREP(ATTR_FEAT_ID_MASK, tunable_attr->feature_id) |
		FIELD_PREP(ATTR_MODE_ID_MASK, SMARTFAN_MODE_CUSTOM) |
		FIELD_PREP(ATTR_TYPE_ID_MASK, tunable_attr->type_id);

	struct lenovo_wmi_om_priv *priv;
	u32 value;
	int err;

	err = kstrtouint(buf, 10, &value);
	if (err) {
		pr_debug("Error converting value to int: %d\n", err);
		return err;
	}

	if (value < tunable_attr->capdata->min_value ||
	    value > tunable_attr->capdata->max_value)
		return -EINVAL;

	priv = get_first_wmi_priv();
	if (!priv)
		return -ENODEV;

	err = lenovo_wmidev_evaluate_method_2(priv->wdev, 0x0,
					      WMI_FEATURE_VALUE_SET,
					      attribute_id, value, NULL);

	if (err) {
		pr_debug("Error setting attribute: %d\n", err);
		return err;
	}

	tunable_attr->store_value = value;

	sysfs_notify(kobj, NULL, attr->attr.name);

	return count;
};

/*
 * attr_current_value_show() - Get the current value of the given attribute
 * @kobj: Pointer to the driver object.
 * @kobj_attribute: Pointer to the attribute calling this function.
 * @buf: The buffer to write to.
 * @tunable_attr: The attribute to be read.
 *
 * This function is intended to be generic so it can be called from any "_show"
 * attribute which works only with integers.
 *
 * If the WMI is success, then the sysfs attribute is notified.
 *
 * Returns: Either count, or an error.
 */
static ssize_t attr_current_value_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf,
				       struct tunable_attr_01 *tunable_attr)
{
	u32 attribute_id =
		FIELD_PREP(ATTR_DEV_ID_MASK, tunable_attr->device_id) |
		FIELD_PREP(ATTR_FEAT_ID_MASK, tunable_attr->feature_id) |
		FIELD_PREP(ATTR_MODE_ID_MASK, SMARTFAN_MODE_CUSTOM) |
		FIELD_PREP(ATTR_TYPE_ID_MASK, tunable_attr->type_id);

	struct lenovo_wmi_om_priv *priv;
	int retval;
	int err;

	priv = get_first_wmi_priv();
	if (!priv)
		return -ENODEV;

	err = lenovo_wmidev_evaluate_method_1(
		priv->wdev, 0x0, WMI_FEATURE_VALUE_GET, attribute_id, &retval);

	if (err) {
		pr_debug("Error getting attribute: %d\n", err);
		return err;
	}

	return sysfs_emit(buf, "%d\n", retval);
}

/* Attribute macros */
#define __LL_ATTR_RO(_func, _name)                                    \
	{                                                             \
		.attr = { .name = __stringify(_name), .mode = 0444 }, \
		.show = _func##_##_name##_show,                       \
	}

#define __LL_ATTR_RO_AS(_name, _show)                                 \
	{                                                             \
		.attr = { .name = __stringify(_name), .mode = 0444 }, \
		.show = _show,                                        \
	}

#define __LL_ATTR_RW(_func, _name) \
	__ATTR(_name, 0644, _func##_##_name##_show, _func##_##_name##_store)

/* Shows a formatted static variable */
#define __ATTR_SHOW_FMT(_prop, _attrname, _fmt, _val)                         \
	static ssize_t _attrname##_##_prop##_show(                            \
		struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
	{                                                                     \
		return sysfs_emit(buf, _fmt, _val);                           \
	}                                                                     \
	static struct kobj_attribute attr_##_attrname##_##_prop =             \
		__LL_ATTR_RO(_attrname, _prop)

/* Attribute current_value show/store */
#define __LL_TUNABLE_RW_CAP01(_attrname)                                      \
	static ssize_t _attrname##_current_value_store(                       \
		struct kobject *kobj, struct kobj_attribute *attr,            \
		const char *buf, size_t count)                                \
	{                                                                     \
		return attr_current_value_store(kobj, attr, buf, count,       \
						&_attrname);                  \
	}                                                                     \
	static ssize_t _attrname##_current_value_show(                        \
		struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
	{                                                                     \
		return attr_current_value_show(kobj, attr, buf, &_attrname);  \
	}                                                                     \
	static struct kobj_attribute attr_##_attrname##_current_value =       \
		__LL_ATTR_RW(_attrname, current_value)

/* Attribute property show only */
#define __LL_TUNABLE_RO_CAP01(_prop, _attrname, _prop_type)                   \
	static ssize_t _attrname##_##_prop##_show(                            \
		struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
	{                                                                     \
		return attr_capdata01_show(kobj, attr, buf, &_attrname,       \
					   _prop_type);                       \
	}                                                                     \
	static struct kobj_attribute attr_##_attrname##_##_prop =             \
		__LL_ATTR_RO(_attrname, _prop)

#define ATTR_GROUP_LL_TUNABLE_CAP01(_attrname, _fsname, _dispname)     \
	__LL_TUNABLE_RW_CAP01(_attrname);                              \
	__LL_TUNABLE_RO_CAP01(default_value, _attrname, DEFAULT_VAL);  \
	__ATTR_SHOW_FMT(display_name, _attrname, "%s\n", _dispname);   \
	__LL_TUNABLE_RO_CAP01(max_value, _attrname, MAX_VAL);          \
	__LL_TUNABLE_RO_CAP01(min_value, _attrname, MIN_VAL);          \
	__LL_TUNABLE_RO_CAP01(scalar_increment, _attrname, STEP_VAL);  \
	static struct kobj_attribute attr_##_attrname##_type =         \
		__LL_ATTR_RO_AS(type, int_type_show);                  \
	static struct attribute *_attrname##_attrs[] = {               \
		&attr_##_attrname##_current_value.attr,                \
		&attr_##_attrname##_default_value.attr,                \
		&attr_##_attrname##_display_name.attr,                 \
		&attr_##_attrname##_max_value.attr,                    \
		&attr_##_attrname##_min_value.attr,                    \
		&attr_##_attrname##_scalar_increment.attr,             \
		&attr_##_attrname##_type.attr,                         \
		NULL,                                                  \
	};                                                             \
	static const struct attribute_group _attrname##_attr_group = { \
		.name = _fsname, .attrs = _attrname##_attrs            \
	}

ATTR_GROUP_LL_TUNABLE_CAP01(ppt_pl1_spl, "ppt_pl1_spl",
			    "Set the CPU sustained power limit");
ATTR_GROUP_LL_TUNABLE_CAP01(ppt_pl2_sppt, "ppt_pl2_sppt",
			    "Set the CPU slow package power tracking limit");
ATTR_GROUP_LL_TUNABLE_CAP01(ppt_pl3_fppt, "ppt_pl3_fppt",
			    "Set the CPU fast package power tracking limit");

static const struct capdata01_attr_group capdata01_attr_groups[] = {
	{ &ppt_pl1_spl_attr_group, &ppt_pl1_spl },
	{ &ppt_pl2_sppt_attr_group, &ppt_pl2_sppt },
	{ &ppt_pl3_fppt_attr_group, &ppt_pl3_fppt },
	{},
};

/**                                      .
 * attr_capdata01_setup() - Get the data of the specified attribute
 * from LENOVO_CAPABILITY_DATA_01 and store it.
 * @tunable_attr: The attribute to be populated.
 *
 * Returns: Either 0 or an error.
 */
static int attr_capdata01_setup(struct tunable_attr_01 *tunable_attr)
{
	u32 attribute_id =
		FIELD_PREP(ATTR_DEV_ID_MASK, tunable_attr->device_id) |
		FIELD_PREP(ATTR_FEAT_ID_MASK, tunable_attr->feature_id) |
		FIELD_PREP(ATTR_MODE_ID_MASK, SMARTFAN_MODE_CUSTOM) |
		FIELD_PREP(ATTR_TYPE_ID_MASK, tunable_attr->type_id);

	tunable_attr->capdata = lenovo_wmi_capdata01_get(attribute_id);
	if (!tunable_attr->capdata)
		return -ENODEV;

	if (tunable_attr->capdata->supported == 0) {
		pr_info("No support!");
		return -EOPNOTSUPP;
	}

	return 0;
}

/**
 * other_method_fan_table_get - Get the current fan table data
 * @kobj: Pointer to the driver object.
 * @kobj_attribute: Pointer to the attribute calling this function.
 * @buf: The buffer to write to.
 */
static int other_method_fan_table_get(int *retval)
{
	struct lenovo_wmi_om_priv *priv;
	int err;

	priv = get_first_wmi_priv();
	if (!priv)
		return -ENODEV;

	err = lenovo_wmidev_evaluate_method_1(priv->wdev, 0x0,
					      WMI_FAN_TABLE_GET, 0x01, retval);

	return err;
}

/**
 * other_method_fan_table_set - Set the current fan table data
 */
static int other_method_fan_table_set(struct set_fan_table_data *table_data)
{
	struct lenovo_wmi_om_priv *priv;
	int retval;
	int err;

	priv = get_first_wmi_priv();
	if (!priv)
		return -ENODEV;

	err = lenovo_wmidev_evaluate_method_64(
		priv->wdev, 0x0, WMI_FAN_TABLE_SET, table_data, &retval);

	return err;
}

static int other_method_fw_attr_add(struct lenovo_wmi_om_priv *priv)
{
	int err, i;

	err = fw_attributes_class_get(&fw_attr_class);
	if (err) {
		pr_debug("Failed to get firmware_attributes_class: %d\n", err);
		return err;
	}
	pr_info("Got fw class.\n");

	priv->fw_attr_dev = device_create(fw_attr_class, NULL, MKDEV(0, 0),
					  NULL, "%s", FW_ATTR_FOLDER);
	if (IS_ERR(priv->fw_attr_dev)) {
		err = PTR_ERR(priv->fw_attr_dev);
		pr_debug(
			"Failed to create firmware_attributes_class device: %d\n",
			err);
		goto fail_class_get;
	}
	pr_info("Got fw dev.\n");

	priv->fw_attr_kset = kset_create_and_add("attributes", NULL,
						 &priv->fw_attr_dev->kobj);
	if (!priv->fw_attr_kset) {
		err = -ENOMEM;
		pr_debug(
			"Failed to create firmware_attributes_class kset: %d\n",
			err);
		goto err_destroy_classdev;
	}
	pr_info("Got kset.\n");

	for (i = 0; i < ARRAY_SIZE(capdata01_attr_groups) - 1; i++) {
		pr_info("Start setup for attribute group %d.\n", i);
		err = attr_capdata01_setup(
			capdata01_attr_groups[i].tunable_attr);
		if (err) {
			pr_debug(
				"Failed to populate capability data for attribute group %d: %d\n",
				i, err);
			continue;
		}
		pr_info("Got capability data for attribute group %d.\n", i);

		err = sysfs_create_group(&priv->fw_attr_kset->kobj,
					 capdata01_attr_groups[i].attr_group);
		if (err) {
			pr_debug("Failed to create sysfs-group for %s: %d\n",
				 capdata01_attr_groups[i].attr_group->name,
				 err);
			goto err_remove_groups;
		}
		pr_info("Got sysfs group for attribute group %d.\n", i);
	}
	pr_info("Setup completed.\n");

	return 0;

err_remove_groups:
	while (i-- > 0) {
		sysfs_remove_group(&priv->fw_attr_kset->kobj,
				   capdata01_attr_groups[i].attr_group);
	}
	kset_unregister(priv->fw_attr_kset);

err_destroy_classdev:
	device_unregister(priv->fw_attr_dev);

fail_class_get:
	fw_attributes_class_put();
	return err;
}

static int lenovo_wmi_other_probe(struct wmi_device *wdev, const void *context)
{
	struct lenovo_wmi_om_priv *priv;
	pr_info("Probe Start.\n");

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	pr_info("Allocated Priv.\n");
	priv->wdev = wdev;

	dev_set_drvdata(&wdev->dev, priv);
	pr_info("Set  drvdata.\n");

	guard(mutex)(&om_list_mutex);
	list_add_tail(&priv->list, &om_wmi_list);
	pr_info("Added to list.\n");

	return other_method_fw_attr_add(priv);
}

static void lenovo_wmi_other_remove(struct wmi_device *wdev)
{
	struct lenovo_wmi_om_priv *priv = dev_get_drvdata(&wdev->dev);

	guard(mutex)(&om_list_mutex);
	list_del(&priv->list);
	kset_unregister(priv->fw_attr_kset);
	device_destroy(fw_attr_class, MKDEV(0, 0));
	fw_attributes_class_put();
}

static const struct wmi_device_id lenovo_wmi_other_id_table[] = {
	{ LENOVO_OTHER_METHOD_GUID, NULL },
	{}
};

static struct wmi_driver lenovo_wmi_other_driver = {
	.driver = { .name = "lenovo_wmi_other" },
	.id_table = lenovo_wmi_other_id_table,
	.probe = lenovo_wmi_other_probe,
	.remove = lenovo_wmi_other_remove,
};

module_wmi_driver(lenovo_wmi_other_driver);

MODULE_IMPORT_NS("CAPDATA_WMI");
MODULE_IMPORT_NS("LENOVO_WMI");
MODULE_DEVICE_TABLE(wmi, lenovo_wmi_other_id_table);
MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_DESCRIPTION("Lenovo Other Method WMI Driver");
MODULE_LICENSE("GPL");
