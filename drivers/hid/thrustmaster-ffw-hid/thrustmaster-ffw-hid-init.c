// SPDX-License-Identifier: GPL-2.0
/*
 * When connected to the machine, some Thrustmaster wheels appear as
 * a genric hid gamepad called "Thrustmaster FFB Wheel". 
 *
 * While in this mode, force feedback is not available and the device report 
 * decriptor falls back to a basic input mode. To enable all functionalities
 * of the wheel a specific USB CONTROL request is sent.
 *
 * This driver identifies the true model of Thrustmaster wheel and then sends
 * the appropriate USB CONTROL request to switch to the full operating mode
 * for that device.
 *
 * Copyright (c) 2020-2021 Dario Pagani <dario.pagani.146+linuxk@gmail.com>
 * Copyright (c) 2020-2021 Kim Kuparinen <kimi.h.kuparinen@gmail.com>
 * Copywrite (c) 2025 Derek J. Clark <derekjohn.clark@gmail.com>
 */

#include "linux/device/devres.h"
#include <linux/hid.h>
#include <linux/usb.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/module.h>

#include "thrustmaster-ffw-hid-core.h"
#include "thrustmaster-ffw-hid-init.h"

/*
 * These interrupts are used to prevent a nasty crash when initializing the
 * T300RS. Used in thrustmaster_interrupts().
 */
static const u8 setup_0[] = { 0x42, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const u8 setup_1[] = { 0x0a, 0x04, 0x90, 0x03, 0x00, 0x00, 0x00, 0x00 };
static const u8 setup_2[] = { 0x0a, 0x04, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00 };
static const u8 setup_3[] = { 0x0a, 0x04, 0x12, 0x10, 0x00, 0x00, 0x00, 0x00 };
static const u8 setup_4[] = { 0x0a, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00 };
static const u8 *const setup_arr[] = { setup_0, setup_1, setup_2, setup_3, setup_4 };
static const unsigned int setup_arr_sizes[] = {
	ARRAY_SIZE(setup_0),
	ARRAY_SIZE(setup_1),
	ARRAY_SIZE(setup_2),
	ARRAY_SIZE(setup_3),
	ARRAY_SIZE(setup_4)
};

struct tmffw_init_info {
	uint16_t wheel_type;
	uint16_t switch_value;
	char const *const wheel_name;
};

static const struct tmffw_init_info tm_wheels_infos[] = {
	{0x0002, 0x0002, "Thrustmaster T500RS"},
	{0x0200, 0x0005, "Thrustmaster T300RS (Missing Attachment)"},
	{0x0204, 0x0005, "Thrustmaster T300 Ferrari Alcantara Edition"},
	{0x0206, 0x0005, "Thrustmaster T300RS"},
	{0x0209, 0x0005, "Thrustmaster T300RS (Open Wheel Attachment)"},
	{0x020a, 0x0005, "Thrustmaster T300RS (Sparco R383 Mod)"},
	{0x0306, 0x0006, "Thrustmaster T150RS"},
	{0x0609, 0x0009, "Thrustmaster TS-PC"},
};

static const uint8_t tm_wheels_infos_length = 7;

struct tm_wheel {
	struct usb_device *usb_dev;
	struct urb *urb;

	struct usb_ctrlrequest *model_request;
	struct tmff_urb_response *response;

	struct usb_ctrlrequest *change_request;
};

/* The control packet to send to wheel */
static const struct usb_ctrlrequest model_request = {
	.bRequestType = 0xc1,
	.bRequest = 73,
	.wValue = 0,
	.wIndex = 0,
	.wLength = cpu_to_le16(0x0010)
};

static const struct usb_ctrlrequest change_request = {
	.bRequestType = 0x41,
	.bRequest = 83,
	.wValue = 0, // Will be filled by the driver
	.wIndex = 0,
	.wLength = 0
};

/*
 * On some setups initializing the T300RS crashes the kernel,
 * these interrupts fix that particular issue. So far they haven't caused any
 * adverse effects in other wheels.
 */
static void tmffw_init_interrupts(struct hid_device *hdev)
{
	struct usb_interface *usbif = to_usb_interface(hdev->dev.parent);
	struct usb_device *usbdev = interface_to_usbdev(usbif);
	struct usb_host_endpoint *ep;
	int ret, trans, i, b_ep;
	u8 *send_buf = NULL;
	
	send_buf = devm_kmalloc(&hdev->dev, 256, GFP_KERNEL);
	if (!send_buf) {
		hid_err(hdev, "failed allocating send buffer\n");
		return;
	}

	if (usbif->cur_altsetting->desc.bNumEndpoints < 2) {
		hid_err(hdev, "Wrong number of endpoints?\n");
		return;
	}

	ep = &usbif->cur_altsetting->endpoint[1];
	b_ep = ep->desc.bEndpointAddress;

	u8 ep_addr[2] = {b_ep, 0};

	if (!usb_check_int_endpoints(usbif, ep_addr)) {
		hid_err(hdev, "Unexpected non-int endpoint\n");
		return;
	}

	for (i = 0; i < ARRAY_SIZE(setup_arr); ++i) {
		memcpy(send_buf, setup_arr[i], setup_arr_sizes[i]);

		ret = usb_interrupt_msg(usbdev,
			usb_sndintpipe(usbdev, b_ep),
			send_buf,
			setup_arr_sizes[i],
			&trans,
			USB_CTRL_SET_TIMEOUT);

		if (ret) {
			hid_err(hdev, "setup data couldn't be sent\n");
			return;
		}
	}
}

static void tmffw_init_change_handler(struct urb *urb)
{
	struct hid_device *hdev = urb->context;
	
	// The USB HID device disconnects before answering the host, ignore.
	if (urb->status == 0 || urb->status == -EPROTO ||
	    urb->status == -EPIPE || urb->status == -ESHUTDOWN) 
		hid_info(hdev, "Initialized Thrustmaster Wheel mode change\n");
	else
		hid_err(hdev, "URB to change wheel mode failed with error %d\n", urb->status);
}

/*
 * Called by the USB subsystem when the wheel responses to our request
 * to get [what it seems to be] the wheel's model.
 *
 * If the model id is recognized then we send an opportune USB CONTROL REQUEST
 * to switch the wheel to its full capabilities
 */
static void tmffw_init_model_handler(struct urb *urb)
{
	const struct tmffw_init_info *twi = NULL;
	struct tmffw_drvdata *tm_wheel = urb->context;
	uint16_t model = 0;
	int i, ret;

	if (urb->status) {
		hid_err(tm_wheel->hdev, "Get model id URB request failed with error %d\n", urb->status);
		return;
	}

	if (tm_wheel->response->type == cpu_to_le16(0x49))
		model = le16_to_cpu(tm_wheel->response->data.a.model);
	else if (tm_wheel->response->type == cpu_to_le16(0x47))
		model = le16_to_cpu(tm_wheel->response->data.b.model);
	else {
		hid_err(tm_wheel->hdev, "Unknown packet type 0x%x, unable complete init\n", tm_wheel->response->type);
		return;
	}

	for (i = 0; i < tm_wheels_infos_length && !twi; i++)
		if (tm_wheels_infos[i].wheel_type == model)
			twi = tm_wheels_infos + i;

	if (twi)
		hid_info(tm_wheel->hdev, "Wheel with model id 0x%x is a %s\n", model, twi->wheel_name);
	else {
		hid_err(tm_wheel->hdev, "Unknown wheel's model id 0x%x, unable to proceed further with wheel init\n", model);
		return;
	}

	tm_wheel->change_request->wValue = cpu_to_le16(twi->switch_value);
	usb_fill_control_urb(
		urb,
		tm_wheel->usb_dev,
		usb_sndctrlpipe(tm_wheel->usb_dev, 0),
		(char *)tm_wheel->change_request,
		NULL, 0,
		tmffw_init_change_handler,
		tm_wheel->hdev
	);

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret)
		hid_err(tm_wheel->hdev, "Error while submitting mode change URB request: %d\n", ret);
	
	usb_free_urb(urb);
}


/*
 */
int tmffw_init_probe(struct tmffw_drvdata *tm_wheel)
{
	struct urb *urb;
	int ret = 0;

	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!urb) {
		ret = -ENOMEM;
		goto err_exit;
	}

	tm_wheel->model_request = kmemdup(&model_request,
					  sizeof(struct usb_ctrlrequest),
					  GFP_KERNEL);
	if (!tm_wheel->model_request) {
		ret = -ENOMEM;
		goto err_free_urb;
	}

	tm_wheel->response = devm_kzalloc(&tm_wheel->hdev->dev, sizeof(struct tmff_urb_response), GFP_KERNEL);
	if (!tm_wheel->response) {
		ret = -ENOMEM;
		goto err_free_urb;
	}

	tm_wheel->change_request = kmemdup(&change_request,
					   sizeof(struct usb_ctrlrequest),
					   GFP_KERNEL);
	if (!tm_wheel->change_request) {
		ret = -ENOMEM;
		goto err_free_urb;
	}

	tmffw_init_interrupts(tm_wheel->hdev);

	usb_fill_control_urb(
		urb,
		tm_wheel->usb_dev,
		usb_rcvctrlpipe(tm_wheel->usb_dev, 0),
		(char *)tm_wheel->model_request,
		tm_wheel->response,
		sizeof(struct tmff_urb_response),
		tmffw_init_model_handler,
		tm_wheel
	);

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret)
		goto err_free_urb;

	return ret;

err_free_urb:
	usb_free_urb(urb);
err_exit:
	return ret;
}
