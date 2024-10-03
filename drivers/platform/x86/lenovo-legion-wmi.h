// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Lenovo Legion WMI interface driver. The Lenovo Legion WMI interface is
 * broken up into multiple GUID interfaces that require cross-references 
 * between GUID's for some functionality. The "Custom Mode" interface is a 
 * legacy interface for managing and displaying CPU & GPU power and hwmon
 * settings and readings. The "Other Mode" interface is a modern interface 
 * that replaces or extends the "Custom Mode" interface. The "GameZone"
 * interface adds advanced features such as fan profiles and overclocking.
 * The "Lighting" interface adds control of various status lights related to 
 * different hardware components. This driver acts as a repository of common
 * functinality as well as a link between the various GUID methods.
 *
 * Copyright(C) 2024 Derek J. Clark <derekjohn.clark@gmail.com>
 *
 */

#include <linux/acpi.h>
#include <linux/wmi.h>

#define LENOVO_GAMEZONE_GUID "887B54E3-DDDC-4B2C-8B88-68A26A8835D0"
#define LENOVO_OTHER_METHOD_GUID "DC2A8805-3A8C-41BA-A6F7-092E0089CD3B"

/* Power Limit */
#define WMI_METHOD_ID_SPPT 0x0101 /* Short Term Power Limit */
#define WMI_METHOD_ID_FPPT 0x0102 /* Long Term Power Limit */
#define WMI_METHOD_ID_SPL 0x0103 /* Peak Power Limit */
//#define WMI_METHOD_ID_CPU_TEMP   0x0104  /* CPU Temp Limit */
//#define WMI_METHOD_ID_APU_SPPT   0x0105  /* APU Slow Power Limit */ /* Intel? */
//#define WMI_METHOD_ID_CPU_PL     0x0106  /* Cross Loading CPU Power Limit */ /* Intel? */

/* Platform Profile */
#define WMI_METHOD_ID_SMARTFAN_SUPP 43 /* IsSupportSmartFan */
#define WMI_METHOD_ID_SMARTFAN_SET 44 /* SetSmartFanMode */
#define WMI_METHOD_ID_SMARTFAN_GET 45 /* GetSmartFanMode */
#define SMARTFAN_MODE_QUIET 0x01
#define SMARTFAN_MODE_BALANCED 0x02
#define SMARTFAN_MODE_PERFORMANCE 0x03
#define SMARTFAN_MODE_CUSTOM 0xFF /* Enables power limit setting */

/* static int lenovo_legion_evaluate_method_2(struct wmi_device *wdev,
					       u8 instance, u32 method_id,
					       u32 arg0, u32 arg1, u32 *retval);

static int lenovo_legion_evaluate_method_1(struct wmi_device *wdev, u8 instance,
					   u32 method_id, u32 arg0,
					   u32 *retval);
*/
