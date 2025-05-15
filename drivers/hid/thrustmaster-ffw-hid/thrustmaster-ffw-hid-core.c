// SPDX-License-Identifier: GPL-2.0
/*
 * When connected to the machine, the Thrustmaster wheels appear as
 * a «generic» hid gamepad called "Thrustmaster FFB Wheel".
 *
 * When in this mode not every functionality of the wheel, like the force feedback,
 * are available. To enable all functionalities of a Thrustmaster wheel we have to send
 * to it a specific USB CONTROL request with a code different for each wheel.
 *
 * This driver tries to understand which model of Thrustmaster wheel the generic
 * "Thrustmaster FFB Wheel" really is and then sends the appropriate control code.
 *
 * Copyright (c) 2020-2021 Dario Pagani <dario.pagani.146+linuxk@gmail.com>
 * Copyright (c) 2020-2021 Kim Kuparinen <kimi.h.kuparinen@gmail.com>
 * Copywrite (c) 2025 Derek J. Clark <derekjohn.clark@gmail.com>
 */
#include <linux/device/devres.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "../hid-ids.h"
#include "thrustmaster-ffw-hid-core.h"
#include "thrustmaster-ffw-hid-init.h"
#include "thrustmaster-ffw-hid-tmff.h"

struct tmffw_init_info {
	uint16_t pid;
	enum tmffw_if_type iface_t;
	char const *const name;
	enum legacy_ff_effect ff_effect;
};

// RW Attributes
// gain 0-65535
// autocenter 0-65535
// autocenter_enable 0-1
// range 0-900/1080
// spring_level 0-100
// damper_level 0-100
// friction_level 0-100
// open_mode 0-1
// timer_msecs ?


/* Keep in PID numerical order */
static const struct tmffw_init_info tm_wheels[] = {

	{ 0xb300, FFW_LEGACY, "Thrustmaster Wheel", EFFECT_RUMBLE },
	{ 0xb304, FFW_LEGACY, "FireStorm Dual Power 2 (and 3)", EFFECT_RUMBLE },
	{ 0xb320, FFW_LEGACY, "Dual Trigger 2-in-1", EFFECT_RUMBLE },
	{ 0xb323, FFW_LEGACY, "Dual Trigger 3-in-1 (PC Mode)", EFFECT_RUMBLE },
	{ 0xb324, FFW_LEGACY, "Dual Trigger 3-in-1 (PS3 Mode)", EFFECT_RUMBLE },
	{ 0xb605, FFW_LEGACY, "NASCAR PRO FF2 Wheel", EFFECT_CONSTANT },
	{ 0xb651, FFW_LEGACY, "FGT Rumble Force Wheel", EFFECT_RUMBLE },
	{ 0xb653, FFW_LEGACY, "RGT Force Feedback CLUTCH Raging Wheel", EFFECT_CONSTANT },
	{ 0xb654, FFW_LEGACY, "FGT Force Feedback Wheel", EFFECT_CONSTANT},
	{ 0xb65a, FFW_LEGACY, "F430 Force Feedback Wheel", EFFECT_CONSTANT },
	{ 0xb65d, FFW_SETUP, "Thrustmaster FFB Wheel", EFFECT_NONE },
	{ 0xb65e, FFW_T500, "TRS Racing wheel", EFFECT_NONE },
	{ 0xb669, FFW_T300, "Thrustmaster TX Racing Wheel", EFFECT_NONE  },
	{ 0xb66d, FFW_T300, "Thrustmaster T300RS", EFFECT_NONE },
	{ 0xb66f, FFW_T300, "Thrustmaster T300RS", EFFECT_NONE },
	{ 0xb677, FFW_T150, "Thrustmaster T150RS Racing Wheel", EFFECT_NONE  },
	{ 0xb67f, FFW_T150, "Thrustmaster TMX Racing Wheel", EFFECT_NONE  },
	{ 0xb689, FFW_T300, "Thrustmaster TS_PC Racing WHeel", EFFECT_NONE },
	{ 0xb692, FFW_T300, "Thrustmaster TX_XW Racing Wheel", EFFECT_NONE  },
	{ 0xb696, FFW_T300, "Thrustmaster T-GT II Racing Wheel", EFFECT_NONE },
	{ 0xb699, FFW_T300, "Thrustmaster T128X Racing Wheel", EFFECT_NONE },
};

static void tmffw_remove(struct hid_device *hdev)
{
	hid_hw_stop(hdev);
}

/*
 * Function called by HID when a hid Thrustmaster FFB wheel is connected to the host.
 * This function starts the hid dev, tries to allocate the tm_wheel data structure and
 * finally send an USB CONTROL REQUEST to the wheel to get [what it seems to be] its
 * model type.
 */
static int tmffw_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct tmffw_drvdata *tmffw_drvdata = NULL;
	enum tmffw_if_type iface_t = FFW_UNKNOWN;
	enum legacy_ff_effect ff_effect;
	int i, ret = 0;

	if (!hid_is_usb(hdev))
		return -EINVAL;

	ret = hid_parse(hdev);
	if (ret)
		goto err_exit;

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT & ~HID_CONNECT_FF);
	if (ret)
		goto err_exit;

	tmffw_drvdata = devm_kzalloc(&hdev->dev, sizeof(struct tmffw_drvdata), GFP_KERNEL);
	if (!tmffw_drvdata) {
		ret = -ENOMEM;
		goto err_stop;
	}

	tmffw_drvdata->hdev = hdev;
	hid_set_drvdata(hdev, tmffw_drvdata);

	tmffw_drvdata->usb_dev = interface_to_usbdev(to_usb_interface(hdev->dev.parent));
	dev_set_drvdata(&tmffw_drvdata->usb_dev->dev, tmffw_drvdata);


	for (i = 0; i < ARRAY_SIZE(tm_wheels); i++)
		if (tm_wheels[i].pid == id->product) {
			iface_t = tm_wheels[i].iface_t;
			ff_effect = tm_wheels[i].ff_effect;
			break;
		}

	switch (iface_t) {
	case FFW_LEGACY:
		tmff_init(hdev, ff_effect);
		break;
	case FFW_SETUP:
		ret = tmffw_init_probe(tmffw_drvdata);
		if (ret)
			goto err_stop;
		break;
	case FFW_T150:
		dev_info(&hdev->dev, "FFW_T150 family device Force Feedback features are not yet supported by this driver.\n");
		break;
	case FFW_T300:
		dev_info(&hdev->dev, "FFW_T300 family device Force Feedback features are not yet supported by this driver.\n");
		break;
	case FFW_T500:
		dev_info(&hdev->dev, "FFW_T300 family device Force Feedback features are not yet supported by this driver.\n");
		break;
	case FFW_UNKNOWN:
		ret = -ENODEV;
		goto err_stop;
		break;
	}

	return 0;

err_stop:
	hid_hw_stop(hdev);
err_exit:
	return ret;
}

static const struct hid_device_id tmffw_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb300) }, /* Firestorm Dual Power */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb304) }, /* FireStorm Dual Power 2 (and 3) */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb320) }, /* Dual Trigger 2-in-1 */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb323) }, /* Dual Trigger 3-in-1 (PC Mode) */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb324) }, /* Dual Trigger 3-in-1 (PS3 Mode) */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb605) }, /* NASCAR PRO FF2 Wheel */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb651) }, /* FGT Rumble Force Wheel */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb653) }, /* RGT Force Feedback CLUTCH Raging Wheel */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb654) }, /* FGT Force Feedback Wheel */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb65a) }, /* F430 Force Feedback Wheel */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb65d) }, /* GIP Init Mode */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb65e) }, /* T500RS */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb669) }, /* TX */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb66d) }, /* T300RS PS4 */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb66f) }, /* T300RS PS3 Advanced */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb677) }, /* T150RS */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb67f) }, /* TMX */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb689) }, /* TS_PC */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb692) }, /* TS_XW */
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb696) }, /* T128, T248 PC, T818, & T-GT II*/
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, 0xb699) }, /* T128X */
	{ },
};

MODULE_DEVICE_TABLE(hid, tmffw_devices);

static struct hid_driver tmffw_driver = {
	.name = "thustmaster-ffw-hid",
	.id_table = tmffw_devices,
	.probe = tmffw_probe,
	.remove = tmffw_remove,
};

module_hid_driver(tmffw_driver);

MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver for Thrustmaster Racing Wheels and Joysticks");

