/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Lenovo Legion WMI interface driver. The Lenovo Legion WMI interface is
 * broken up into multiple GUID interfaces that require cross-references
 * between GUID's for some functionality. The "Custom Mode" interface is a
 * legacy interface for managing and displaying CPU & GPU power and hwmon
 * settings and readings. The "Other Mode" interface is a modern interface
 * that replaces or extends the "Custom Mode" interface methods. The
 * "GameZone" interface adds advanced features such as fan profiles and
 * overclocking. The "Lighting" interface adds control of various status
 * lights related to different hardware components. "Other Method" uses
 * the data structs LENOVO_CAPABILITY_DATA_00, LENOVO_CAPABILITY_DATA_01
 * and LENOVO_CAPABILITY_DATA_02 structs for capability information.
 *
 * Copyright(C) 2024 Derek J. Clark <derekjohn.clark@gmail.com>
 *
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#ifndef _LENOVO_WMI_H_
#define _LENOVO_WMI_H_

#include <linux/bits.h>
#include <linux/types.h>
#include <linux/wmi.h>

/* Platform Profile Modes */
#define SMARTFAN_MODE_QUIET 0x01
#define SMARTFAN_MODE_BALANCED 0x02
#define SMARTFAN_MODE_PERFORMANCE 0x03
#define SMARTFAN_MODE_EXTREME 0xE0 /* Ver 6+ */
#define SMARTFAN_MODE_CUSTOM 0xFF /* Ver 7+ */

struct wmi_method_args {
	u32 arg0;
	u32 arg1;
};

enum lenovo_wmi_action {
	SMARTFAN_MODE_EVENT,
};

/* Data struct for LENOVO_CAPABILITY_DATA_01 */
struct capdata01 {
	u32 id;
	u32 supported;
	u32 default_value;
	u32 step;
	u32 min_value;
	u32 max_value;
};

int lenovo_wmidev_evaluate_method_64(struct wmi_device *wdev, u8 instance,
				     u32 method_id, u64 arg, u32 *retval);

int lenovo_wmidev_evaluate_method_2(struct wmi_device *wdev, u8 instance,
				    u32 method_id, u32 arg0, u32 arg1,
				    u32 *retval);

int lenovo_wmidev_evaluate_method_1(struct wmi_device *wdev, u8 instance,
				    u32 method_id, u32 arg0, u32 *retval);

/* LENOVO_GAMEZONE_WMI exported functions */
int lenovo_gz_notifier_call(struct notifier_block *nb, unsigned long action,
			    void *data);
int lenovo_gz_register_notifier(struct notifier_block *nb);
int lenovo_gz_unregister_notifier(struct notifier_block *nb);
int devm_lenovo_gz_register_notifier(struct device *dev,
				     struct notifier_block *nb);
/* LENOVO_CAPABILITY_DATA_01 exported functions */
struct capdata01 *lenovo_wmi_capdata01_get(u32 attribute_id);

#endif /* !_LENOVO_WMI_H_ */
