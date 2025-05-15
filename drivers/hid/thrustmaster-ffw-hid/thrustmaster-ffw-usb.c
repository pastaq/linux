// SPDX-License-Identifier: GPL-2.0
/*
 * When connected, Some Thrustmaster appear as a generic USB gamepad called
 * Thrustmaster <model> GIP Racing Wheel, or similar.
 *
 * While in this mode, force feedback is not available and the device report 
 * decriptor falls back to a basic input mode. To enable all functionalities
 * of the wheel a specific USB CONTROL request is sent.
 *
 * This driver sends the appropriate USB CONTROL request to switch to the
 * full operating mode for each device.
 *
 * Copyright (c) 2025 Derek J. Clark <derekjohn.clark@gmail.com>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device/devres.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

static const struct usb_ctrlrequest change_request = {
	.bRequestType = 0x41,
	.bRequest = 83,
	.wValue = 0, // Filled by PID match
	.wIndex = 0,
	.wLength = 0
};

struct tm_wheel_usb {
	struct usb_ctrlrequest *change_request;
	struct usb_device *usb_dev;
};

struct tm_wheel_usb_info {
	char const *const name;
	uint16_t wValue;
};

/* Wheels use the same firmware and can imitate each other's PID if they
 * get into a bad state. Use the product name which doesn't seem to change.
 */
static const struct tm_wheel_usb_info tm_wheels[] = {
	{ "Thrustmaster T128X GIP Racing Wheel", 0x000b },
	{ "Thrustmaster TMX GIP Racing Wheel", 0x0007 },
	{ "Thrustmaster TS-XW Racer GIP Wheel", 0x000a },
	{ "Thrustmaster TX GIP Racing Wheel", 0x0004 },
};

static void tmffw_usb_change_handler(struct urb *urb)
{
	struct device *dev = urb->context;

	// The USB device disconnects before answering the host, ignore.
	if (urb->status == 0 || urb->status == -EPROTO ||
	    urb->status == -EPIPE || urb->status == -ESHUTDOWN)
		dev_info(dev, "Initialized Thrustmaster Wheel mode change\n");
	else
		dev_err(dev, "URB to change wheel mode failed with error %d\n",
			urb->status);
}

static void tmffw_usb_remove(struct usb_interface *iface)
{
	dev_info(&iface->dev, "Device Removed\n");
}
/*
 * Function called by USB when a USB Thrustmaster FFB wheel is connected to the host.
 * This function tries to allocate the tm_wheel data structure and sends a
 * USB_CONTROL_REQUEST to the wheel to change the device's mode to USBHID.
 */
static int tmffw_usb_probe(struct usb_interface *iface,
			   const struct usb_device_id *id)
{
	struct tm_wheel_usb *tm_wheel = NULL;
	struct usb_device *device;
	struct urb *urb;
	int i, ret = 0;

	tm_wheel = devm_kzalloc(&iface->dev, sizeof(struct tm_wheel_usb),
				GFP_KERNEL);
	if (!tm_wheel) {
		ret = -ENOMEM;
		goto err_return;
	}

	device = interface_to_usbdev(iface);
	if (!device) {
		ret = -ENODEV;
		goto err_return;
	}

	tm_wheel->usb_dev = device;
	dev_set_drvdata(&iface->dev, &tm_wheel);

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		ret = -ENOMEM;
		goto err_return;
	}

	tm_wheel->change_request = kmemdup(
		&change_request, sizeof(struct usb_ctrlrequest), GFP_KERNEL);
	if (!tm_wheel->change_request) {
		ret = -ENOMEM;
		goto err_free_usb;
	}

	for (i = 0; i < ARRAY_SIZE(tm_wheels); i++) {
		dev_info(&iface->dev, "device product to match: %s\n",
			 device->product);
		if (!strcmp(tm_wheels[i].name, device->product)) {
			dev_info(&iface->dev, "Match, %s\n", tm_wheels[i].name);
			tm_wheel->change_request->wValue =
				cpu_to_le16(tm_wheels[i].wValue);
			break;
		}
		dev_info(&iface->dev, "%s not a match\n", device->product);
	}

	if (!tm_wheel->change_request->wValue) {
		ret = -ENODEV;
		goto err_free_usb;
	}

	dev_info(&iface->dev, "PID %x switch_value: %x\n", id->idProduct,
		 tm_wheel->change_request->wValue);

	usb_fill_control_urb(urb, tm_wheel->usb_dev,
			     usb_sndctrlpipe(tm_wheel->usb_dev, 0),
			     (char *)tm_wheel->change_request, NULL, 0,
			     tmffw_usb_change_handler, &iface->dev);

	ret = usb_submit_urb(urb, GFP_KERNEL);

err_free_usb:
	usb_free_urb(urb);
err_return:
	return ret;
}

/* keep in pid numerical order */
static const struct usb_device_id tmffw_usb_devices[] = {
	{ 0, 0x044f, 0xb664 }, // Thrustmaster TX
	{ 0, 0x044f, 0xb67e }, // Thrustmaster TMX
	{ 0, 0x044f, 0xb691 }, // Thrustmaster TS-XW
	{ 0, 0x044f, 0xb69c }, // Thrustmaster T128
	{},
};

MODULE_DEVICE_TABLE(usb, tmffw_usb_devices);

static struct usb_driver tmffw_usb_driver = {
	.name = "thrustmaster-ffw-usb",
	.id_table = tmffw_usb_devices,
	.probe = tmffw_usb_probe,
	.disconnect = tmffw_usb_remove,
};

module_usb_driver(tmffw_usb_driver);

MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver to initialize Thrustmaster TX/TMX Racing Wheels");
