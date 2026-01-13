// SPDX-License-Identifier: GPL-2.0
/*
 * When connected to the machine, modern Thrustmaster wheels appear as
 * a «generic» hid gamepad called "Thrustmaster FFB Wheel".
 *
 * When in this mode, force feedback functionality is not available.
 * To enable all functionalities of a Thrustmaster wheel we have to send
 * to it a specific USB CONTROL request with a code that is different for
 * each wheel.
 *
 * This driver identifies which model of Thrustmaster wheel the generic
 * "Thrustmaster FFB Wheel" really is and then sends the appropriate
 * control code. It also handles legacy thrustmater wheel support for
 * older wheels that only have basic rumble effects.
 *
 * Copyright (c) 2020-2021 Dario Pagani <dario.pagani.146+linuxk@gmail.com>
 * Copyright (c) 2020-2021 Kim Kuparinen <kimi.h.kuparinen@gmail.com>
 * Copywrite (c) 2026 Valve Corporation
 */
#include "linux/dev_printk.h"
#include "linux/device.h"
#include "linux/input.h"
#include <linux/array_size.h>
#include <linux/device/devres.h>
#include <linux/gfp_types.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

#include "hid-ids.h"
#include "hid-thrustmaster.h"

/* Legacy defines */

#define THRUSTMASTER_USAGE_FF (HID_UP_GENDESK | 0xbb)

static const signed short ff_rumble[] = {
	FF_RUMBLE,
	-1
};

static const signed short ff_constant[] = {
	FF_CONSTANT,
	-1
};

enum tmwheel_fam {
	FFW_LEGACY,
	FFW_SETUP,
	FFW_T150,
	FFW_T300,
	FFW_T500,
};

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

/*
 * This struct contains init data for each type of Thrustmaster wheel
 *
 * Note: The values are stored in CPU endianness, the USB protocols always
 * use little endian; the macro cpu_to_le[BIT]() must be used when
 * preparing USB packets and vice-versa.
 *
 * Keep in wheel_type numerical order
 */
static const struct tmwheel_init_info {
	uint16_t wheel_type;
	uint16_t switch_value;
	char const *const wheel_name;
} tm_init_info[] = {
	{0x0002, 0x0002, "Thrustmaster T500RS"},
	{0x0200, 0x0005, "Thrustmaster T300RS (Missing Attachment)"},
	{0x0204, 0x0005, "Thrustmaster T300 Ferrari Alcantara Edition"},
	{0x0206, 0x0005, "Thrustmaster T300RS"},
	{0x0209, 0x0005, "Thrustmaster T300RS (Open Wheel Attachment)"},
	{0x020a, 0x0005, "Thrustmaster T300RS (Sparco R383 Mod)"},
	{0x0306, 0x0006, "Thrustmaster T150RS"},
	{0x0609, 0x0009, "Thrustmaster TS-PC"},
};


/*
 * This struct contains the family data for each known Thrustmaster wheel
 *
 * Legacy devices have a speficied Force Feedback effect that needs to be set
 * during probe. The tmff driver used to carry this data as drvdata, but we need
 * to use that for modern wheels to find our pointers.
 *
 * Keep in PID numerical order
 */
struct thrustmaster_devtype_info {
	uint16_t idProduct;
	enum tmwheel_fam tm_fam;
	char const *const name;
	u16 ff_effect;
} tm_wheels[] = {

	{ USB_DEVICE_ID_TM_FIRE_DP, FFW_LEGACY, "Thrustmaster Wheel", FF_RUMBLE },
	{ USB_DEVICE_ID_TM_FIRE_DP2, FFW_LEGACY, "FireStorm Dual Power 2 (and 3)", FF_RUMBLE },
	{ USB_DEVICE_ID_TM_DT_2IN1, FFW_LEGACY, "Dual Trigger 2-in-1", FF_RUMBLE },
	{ USB_DEVICE_ID_TM_DT_2IN1_PC, FFW_LEGACY, "Dual Trigger 3-in-1 (PC Mode)", FF_RUMBLE },
	{ USB_DEVICE_ID_TM_DT_2IN1_PS3, FFW_LEGACY, "Dual Trigger 3-in-1 (PS3 Mode)", FF_RUMBLE },
	{ USB_DEVICE_ID_TM_NASCAR_PRO_FF2, FFW_LEGACY, "NASCAR PRO FF2 Wheel", FF_CONSTANT },
	{ USB_DEVICE_ID_TM_FGT_RUMBLE, FFW_LEGACY, "FGT Rumble Force Wheel", FF_RUMBLE },
	{ USB_DEVICE_ID_TM_RGT, FFW_LEGACY, "RGT Force Feedback CLUTCH Raging Wheel", FF_CONSTANT },
	{ USB_DEVICE_ID_TM_FGT_FFW, FFW_LEGACY, "FGT Force Feedback Wheel", FF_CONSTANT },
	{ USB_DEVICE_ID_TM_F430, FFW_LEGACY, "F430 Force Feedback Wheel", FF_CONSTANT },
	{ USB_DEVICE_ID_TM_INIT, FFW_SETUP, "Thrustmaster FFB Wheel", 0},
	{ USB_DEVICE_ID_TM_T500RS, FFW_T500, "TRS Racing wheel", 0 },
	{ USB_DEVICE_ID_TM_TX, FFW_T300, "Thrustmaster TX Racing Wheel", 0 },
	{ USB_DEVICE_ID_TM_T300RS_PS4, FFW_T300, "Thrustmaster T300RS", 0 },
	{ USB_DEVICE_ID_TM_T300RS_PS3, FFW_T300, "Thrustmaster T300RS", 0 },
	{ USB_DEVICE_ID_TM_T150RS, FFW_T150, "Thrustmaster T150RS Racing Wheel", 0 },
	{ USB_DEVICE_ID_TM_TMX, FFW_T150, "Thrustmaster TMX Racing Wheel", 0 },
	{ USB_DEVICE_ID_TM_TS_PC, FFW_T300, "Thrustmaster TS_PC Racing WHeel", 0 },
	{ USB_DEVICE_ID_TM_TS_XW, FFW_T300, "Thrustmaster TX_XW Racing Wheel", 0 },
	{ USB_DEVICE_ID_TM_T128, FFW_T300, "Thrustmaster T-GT II Racing Wheel", 0 },
	{ USB_DEVICE_ID_TM_T128X, FFW_T300, "Thrustmaster T128X Racing Wheel", 0 },
};

/*
 * This structs contains (in little endian) the response data
 * of the wheel to the request 73
 *
 * Sufficient research to understand what each field does has not
 * been conducted yet. The position and meaning of fields are a
 * very optimistic guess based on instinct....
 */
struct __packed tm_wheel_response
{
	/*
	 * Seems to be the type of packet
	 * - 0x0049 if is data.a (15 bytes)
	 * - 0x0047 if is data.b (7 bytes)
	 */
	uint16_t type;

	union {
		struct __packed {
			uint16_t field0;
			uint16_t field1;
			/*
			 * Seems to be the model code of the wheel
			 * Read table thrustmaster_wheels to values
			 */
			uint16_t model;

			uint16_t field2;
			uint16_t field3;
			uint16_t field4;
			uint16_t field5;
		} a;
		struct __packed {
			uint16_t field0;
			uint16_t field1;
			uint16_t model;
		} b;
	} data;
};

struct tm_wheel {
	struct usb_ctrlrequest *change_request;
	struct usb_ctrlrequest *model_request;
	struct tm_wheel_response *response;
	enum tmwheel_fam tm_fam;
	struct hid_field *ff_field;
	struct hid_report *report;
	struct hid_device *hdev;
	struct usb_device *udev;
	struct input_dev *idev;
	char dev_path[128];
	void *wheel_intf;
	struct urb *urb;
	u16 *ff_effects;
};

#define T150_OPEN_CMD	0x0442
#define T150_HALT_CMD	0x0542
#define T150_CLOSE_CMD	0x0042

struct tm_t150_wheel_intf {
	__le16 intf_cmd_data[3]; //[T150_OPEN_CMD, T150_HALT_CMD, T150_CLOSE_CMD]
	u8 bInterval_out;
	u8 bInterval_in;
	int pipe_out;
	int pipe_in;
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

/* Legacy functions */
/* Changes values from 0 to 0xffff into values from minimum to maximum */
static inline int tm_legacy_scale_u16(unsigned int in, int minimum, int maximum)
{
	int ret;

	ret = (in * (maximum - minimum) / 0xffff) + minimum;
	if (ret < minimum)
		return minimum;
	if (ret > maximum)
		return maximum;
	return ret;
}

/* Changes values from -0x80 to 0x7f into values from minimum to maximum */
static inline int tm_legacy_scale_s8(int in, int minimum, int maximum)
{
	int ret;

	ret = (((in + 0x80) * (maximum - minimum)) / 0xff) + minimum;
	if (ret < minimum)
		return minimum;
	if (ret > maximum)
		return maximum;
	return ret;
}

static int tm_legacy_ff(struct input_dev *dev, void *data,
		struct ff_effect *effect)
{
	struct hid_device *hid = input_get_drvdata(dev);
	struct tm_wheel *thrustmaster = data;
	struct hid_field *ff_field = thrustmaster->ff_field;
	int x, y;
	int left, right;	/* Rumbling */

	switch (effect->type) {
	case FF_CONSTANT:
		x = tm_legacy_scale_s8(effect->u.ramp.start_level,
					ff_field->logical_minimum,
					ff_field->logical_maximum);
		y = tm_legacy_scale_s8(effect->u.ramp.end_level,
					ff_field->logical_minimum,
					ff_field->logical_maximum);

		dbg_hid("(x, y)=(%04x, %04x)\n", x, y);
		ff_field->value[0] = x;
		ff_field->value[1] = y;
		hid_hw_request(hid, thrustmaster->report, HID_REQ_SET_REPORT);
		break;

	case FF_RUMBLE:
		left = tm_legacy_scale_u16(effect->u.rumble.weak_magnitude,
					ff_field->logical_minimum,
					ff_field->logical_maximum);
		right = tm_legacy_scale_u16(effect->u.rumble.strong_magnitude,
					ff_field->logical_minimum,
					ff_field->logical_maximum);

		/* 2-in-1 strong motor is left */
		if (hid->product == USB_DEVICE_ID_TM_DT_2IN1)
			swap(left, right);

		dbg_hid("(left,right)=(%08x, %08x)\n", left, right);
		ff_field->value[0] = left;
		ff_field->value[1] = right;
		hid_hw_request(hid, thrustmaster->report, HID_REQ_SET_REPORT);
		break;
	}
	return 0;
}

static int tm_legacy_probe(struct tm_wheel *tm_wheel)
{
	const unsigned short *ff_bits;
	struct hid_report *report;
	struct list_head *report_list;
	struct hid_input *hidinput;
	struct input_dev *input_dev;
	int i;

	switch (tm_wheel->ff_effects[0]) {
	case FF_RUMBLE:
		ff_bits = ff_rumble;
		break;
	case FF_CONSTANT:
		ff_bits = ff_constant;
		break;
	default:
		return -EINVAL;
	}
	ff_bits = &tm_wheel->ff_effects[0];

	if (list_empty(&tm_wheel->hdev->inputs)) {
		hid_err(tm_wheel->hdev, "no inputs found\n");
		return -ENODEV;
	}
	hidinput = list_entry(tm_wheel->hdev->inputs.next, struct hid_input, list);
	input_dev = hidinput->input;

	tm_wheel = devm_kzalloc(&tm_wheel->hdev->dev, sizeof(struct tm_wheel), GFP_KERNEL);
	if (!tm_wheel)
		return -ENOMEM;

	/* Find the report to use */
	report_list = &tm_wheel->hdev->report_enum[HID_OUTPUT_REPORT].report_list;
	list_for_each_entry(report, report_list, list) {
		int fieldnum;

		for (fieldnum = 0; fieldnum < report->maxfield; ++fieldnum) {
			struct hid_field *field = report->field[fieldnum];

			if (field->maxusage <= 0)
				continue;

			switch (field->usage[0].hid) {
			case THRUSTMASTER_USAGE_FF:
				if (field->report_count < 2) {
					hid_warn(tm_wheel->hdev, "ignoring FF field with report_count < 2\n");
					continue;
				}

				if (field->logical_maximum ==
						field->logical_minimum) {
					hid_warn(tm_wheel->hdev, "ignoring FF field with logical_maximum == logical_minimum\n");
					continue;
				}

				if (tm_wheel->report && tm_wheel->report != report) {
					hid_warn(tm_wheel->hdev, "ignoring FF field in other report\n");
					continue;
				}

				if (tm_wheel->ff_field && tm_wheel->ff_field != field) {
					hid_warn(tm_wheel->hdev, "ignoring duplicate FF field\n");
					continue;
				}

				tm_wheel->report = report;
				tm_wheel->ff_field = field;

				for (i = 0; ff_bits[i] >= 0; i++)
					set_bit(ff_bits[i], input_dev->ffbit);

				break;

			default:
				hid_warn(tm_wheel->hdev, "ignoring unknown output usage %08x\n",
					 field->usage[0].hid);
				continue;
			}
		}
	}

	if (!tm_wheel->report) {
		hid_err(tm_wheel->hdev, "can't find FF field in output reports\n");
		return -ENODEV;
	}

	return input_ff_create_memless(input_dev, tm_wheel, tm_legacy_ff);
}

/* Modern Init functions */
/*
 * On some setups initializing the T300RS crashes the kernel,
 * these interrupts fix that particular issue. So far they haven't caused any
 * adverse effects in other wheels.
 */
static void tm_init_interrupts(struct hid_device *hdev)
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

static void tm_init_change_handler(struct urb *urb)
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
static void tm_init_handler(struct urb *urb)
{
	const struct tmwheel_init_info *twi = NULL;
	struct tm_wheel *tm_wheel = urb->context;
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

	for (i = 0; i < ARRAY_SIZE(tm_init_info) && !twi; i++)
		if (tm_init_info[i].wheel_type == model)
			twi = tm_init_info + i;

	if (twi)
		hid_info(tm_wheel->hdev, "Wheel with model id 0x%x is a %s\n", model, twi->wheel_name);
	else {
		hid_err(tm_wheel->hdev, "Unknown wheel's model id 0x%x, unable to proceed further with wheel init\n", model);
		return;
	}

	tm_wheel->change_request->wValue = cpu_to_le16(twi->switch_value);
	usb_fill_control_urb(
		urb,
		tm_wheel->udev,
		usb_sndctrlpipe(tm_wheel->udev, 0),
		(char *)tm_wheel->change_request,
		NULL, 0,
		tm_init_change_handler,
		tm_wheel->hdev
	);

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret)
		hid_err(tm_wheel->hdev, "Error while submitting mode change URB request: %d\n", ret);
	
	usb_free_urb(urb);
}

/*
 * Sends a USB CONTROL REQUEST to the wheel to get its model type & accessories.
 */
static int tm_init_probe(struct tm_wheel *tm_wheel)
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

	tm_wheel->response = devm_kzalloc(&tm_wheel->hdev->dev, sizeof(struct tm_wheel_response), GFP_KERNEL);
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

	tm_init_interrupts(tm_wheel->hdev);

	usb_fill_control_urb(
		urb,
		tm_wheel->udev,
		usb_rcvctrlpipe(tm_wheel->udev, 0),
		(char *)tm_wheel->model_request,
		tm_wheel->response,
		sizeof(struct tm_wheel_response),
		tm_init_handler,
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


#define tm_wheel_map_key_clear(c)	hid_map_usage_clear(hi, usage, bit, \
						    max, EV_KEY, (c))
static int tm_t150_input_mapping(struct hid_device *hdev,
			      struct hid_input *hi,
			      struct hid_field *field,
			      struct hid_usage *usage,
			      unsigned long **bit,
			      int *max)
{
	unsigned int btn;

	if ((usage->hid & HID_USAGE_PAGE) != HID_UP_BUTTON)
		return 0;

	btn = usage->hid & HID_USAGE;
	if (!btn || btn > ARRAY_SIZE(t150_btn_map))
		return 0;

	tm_wheel_map_key_clear(t150_btn_map[btn - 1]);

	return 1;
}

static int tm_t150_interrupt_msg(struct usb_device *udev, int ep_out, __le16 *msg)
{
	int actual_len, ret;

	ret = usb_interrupt_msg(udev,	ep_out, msg, sizeof(*msg), &actual_len, 8);
	dev_info(&udev->dev, "usb_interrupt_msg wrote message %04x for %x bytes with return code %x\n", *msg, actual_len, ret);
	if (ret)
		return ret;
	return actual_len;

}

static int t150_ff_upload(struct input_dev *dev, struct ff_effect *effect, struct ff_effect *old)
{
	return 0;
}

static int t150_ff_erase(struct input_dev *dev, int effect_id)
{
	return 0;
}

static int t150_ff_play(struct input_dev *dev, int effect_id, int times)
{
	return 0;
}

static void t150_ff_set_gain(struct input_dev *dev, u16 gain)
{
}

static void t150_ff_set_autocenter(struct input_dev *dev, u16 magnitude)
{
}


static int tm_t150_input_open(struct input_dev *dev)
{
	struct tm_wheel *tm_wheel = input_get_drvdata(dev);
	struct tm_t150_wheel_intf *wheel_intf = tm_wheel->wheel_intf;
	int ret;

	dev_info(&tm_wheel->hdev->dev, "tm_t150_input_open\n");
	ret = tm_t150_interrupt_msg(tm_wheel->udev,wheel_intf->pipe_out, &wheel_intf->intf_cmd_data[0]);
	if(ret < 0) {
		dev_err(&dev->dev, "OPEN usb_interrupt_msg failed with return code %x\n", ret);
		return ret;
	};
	dev_info(&dev->dev, "OPEN usb_interrupt_msg wrote %x bytes\n", ret);

	return hid_hw_open(tm_wheel->hdev);
}
static void tm_t150_input_close(struct input_dev *dev)
{
	struct tm_wheel *tm_wheel = input_get_drvdata(dev);
	struct tm_t150_wheel_intf *wheel_intf = tm_wheel->wheel_intf;
	int ret;

	dev_info(&dev->dev, "tm_t150_input_close\n");
	hid_hw_close(tm_wheel->hdev);

	// Send codes for graceful halt (2x) and disconnect
	ret = tm_t150_interrupt_msg(tm_wheel->udev, wheel_intf->pipe_out, &wheel_intf->intf_cmd_data[1]);
	if(ret < 0) {
		dev_err(&dev->dev, "HALT usb_interrupt_msg failed with return code %x\n", ret);
		return;
	};
	dev_info(&dev->dev, "HALT usb_interrupt_msg wrote %x bytes\n", ret);
	tm_t150_interrupt_msg(tm_wheel->udev, wheel_intf->pipe_out, &wheel_intf->intf_cmd_data[1]);
	if(ret < 0) {
		dev_err(&dev->dev, "HALT usb_interrupt_msg failed with return code %x\n", ret);
		return;
	};
	dev_info(&dev->dev, "HALT usb_interrupt_msg wrote %x bytes\n", ret);
	ret = tm_t150_interrupt_msg(tm_wheel->udev, wheel_intf->pipe_out, &wheel_intf->intf_cmd_data[2]);
	if(ret < 0){
		dev_err(&dev->dev, "CLOSE usb_interrupt_msg failed with return code %x\n", ret);
		return;
	};
	dev_info(&dev->dev, "CLOSE usb_interrupt_msg wrote %x bytes\n", ret);
}

static int tm_t150_probe(struct tm_wheel *tm_wheel)
{
	struct usb_interface *uintf = to_usb_interface(tm_wheel->hdev->dev.parent);
	struct usb_endpoint_descriptor *ep;
	struct tm_t150_wheel_intf *wheel_intf;
	struct hid_input *hi;
	int ret, i;

	wheel_intf = devm_kzalloc(&tm_wheel->hdev->dev, sizeof(struct tm_t150_wheel_intf), GFP_KERNEL);
	if (!wheel_intf)
		return -ENOMEM;

	hi = list_entry(tm_wheel->hdev->inputs.next, struct hid_input, list);
	if (!hi || !hi->input)
		return -ENODEV;

	// Set up wheel_intf
	wheel_intf->intf_cmd_data[0] = cpu_to_le16(T150_OPEN_CMD);
	wheel_intf->intf_cmd_data[1] = cpu_to_le16(T150_HALT_CMD);
	wheel_intf->intf_cmd_data[2] = cpu_to_le16(T150_CLOSE_CMD);

	for (i = 0; i < 2; i++) {
		ep = &uintf->cur_altsetting->endpoint[i].desc;
		if (!ep)
			continue;
		if (!usb_endpoint_xfer_int(ep))
			continue;
		if (usb_endpoint_dir_in(ep)){
			wheel_intf->pipe_in = usb_rcvintpipe(tm_wheel->udev, ep->bEndpointAddress);
			wheel_intf->bInterval_in = ep->bInterval;
		}
		else{
			wheel_intf->pipe_out= usb_sndintpipe(tm_wheel->udev, ep->bEndpointAddress);
			wheel_intf->bInterval_out = ep->bInterval;
		}
	}

	if (!wheel_intf->pipe_in || !wheel_intf->pipe_out)
		return -ENODEV;


	tm_wheel->udev = interface_to_usbdev(uintf);
	tm_wheel->wheel_intf = wheel_intf;

	// Set up input device
	tm_wheel->idev = hi->input;
	tm_wheel->idev->open = tm_t150_input_open;
	tm_wheel->idev->close = tm_t150_input_close;

	//TODO: Do we need these?
	//snprintf(tm_wheel->hdev->uniq, sizeof(tm_wheel->hdev->uniq),
        // "%04x:%04x", tm_wheel->hdev->vendor, tm_wheel->hdev->product);
	//tm_wheel->idev->uniq = tm_wheel->hdev->uniq;

	//usb_make_path(tm_wheel->udev, tm_wheel->dev_path, sizeof(tm_wheel->dev_path) - 8);
	//strlcat(tm_wheel->dev_path, "/input0", sizeof(tm_wheel->dev_path));
	//input_set_capability(tm_wheel->idev, EV_FF, FF_RUMBLE);

	input_set_drvdata(tm_wheel->idev, tm_wheel);

	// Setup FFB
	for (i = 0; i < ARRAY_SIZE(t150_ff_effects); i++)
		set_bit(t150_ff_effects[i], tm_wheel->idev->ffbit);

	// Will hid core do this?
	ret = input_ff_create(tm_wheel->idev, FF_MAX_EFFECTS);
	if (ret)
		return ret;

	tm_wheel->idev->ff->upload = t150_ff_upload;
	tm_wheel->idev->ff->erase = t150_ff_erase;
	tm_wheel->idev->ff->playback = t150_ff_play;
	tm_wheel->idev->ff->set_gain = t150_ff_set_gain;
	tm_wheel->idev->ff->set_autocenter = t150_ff_set_autocenter;

	// Setup sysfs

	return 0;
}

static const __u8 *thrustmaster_report_fixup(struct hid_device *hdev, __u8 *rdesc, unsigned int *rsize)
{
	switch (hdev->product) {
	case USB_DEVICE_ID_TM_T150RS:
	case USB_DEVICE_ID_TM_TMX:
		*rsize = sizeof(t150_rdesc);
		return t150_rdesc;
	default:
		return rdesc;
	}

}

static int thrustmaster_input_mapping(struct hid_device *hdev, struct hid_input *hi,
				      struct hid_field *field, struct hid_usage *usage,
				      unsigned long **bit, int *max)
{
	switch (hdev->product) {
	case USB_DEVICE_ID_TM_T150RS:
	case USB_DEVICE_ID_TM_TMX:
		return tm_t150_input_mapping(hdev, hi, field,
					  usage, bit, max);
	default:
		return 0;
	}
}

static void thrustmaster_remove(struct hid_device *hdev)
{
	hid_hw_stop(hdev);
}

/*
 * Function called by HID when a hid Thrustmaster FFB wheel is connected to the host.
 * This function starts the hid dev and tries to allocate the tm_wheel data structure and
 * finally identifies the wheel and offloads to the appropriate init function.
 */
static int thrustmaster_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct tm_wheel *tm_wheel = NULL;
	int i, ret = 0;

	dev_info(&hdev->dev, "Start thrustmaster_probe for device: %04x, %04x\n", id->vendor, id->product);

	if (!hid_is_usb(hdev))
		return -EINVAL;

	ret = hid_parse(hdev);
	if (ret)
		goto err_exit;

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT & ~HID_CONNECT_FF);
	if (ret)
		goto err_exit;

	tm_wheel = devm_kzalloc(&hdev->dev, sizeof(struct tm_wheel), GFP_KERNEL);
	if (!tm_wheel) {
		ret = -ENOMEM;
		goto err_stop;
	}

	hid_set_drvdata(hdev, tm_wheel);
	dev_set_drvdata(&hdev->dev, tm_wheel);
	tm_wheel->hdev = hdev;

	/* used for sending URBs */
	tm_wheel->udev = interface_to_usbdev(to_usb_interface(hdev->dev.parent));

	for (i = 0; i < ARRAY_SIZE(tm_wheels); i++)
		if (tm_wheels[i].idProduct == id->product) {
			dev_info(&hdev->dev, "Found %s\n", tm_wheels[i].name);
			tm_wheel->tm_fam = tm_wheels[i].tm_fam;
			if (tm_wheels[i].ff_effect) {
				u16 *ff_effect;
				ff_effect= devm_kmalloc(&hdev->dev, sizeof(*ff_effect), GFP_KERNEL);
				if (!ff_effect)
					return -ENOMEM;

				*ff_effect = tm_wheels[i].ff_effect;
				tm_wheel->ff_effects = ff_effect;
			}
			break;
		}

	if (!tm_wheel->tm_fam)
		ret = -ENODEV;

	switch (tm_wheel->tm_fam) {
	case FFW_LEGACY:
		ret = tm_legacy_probe(tm_wheel);
		if (ret)
			goto err_stop;
		break;
	case FFW_SETUP:
		ret = tm_init_probe(tm_wheel);
		if (ret)
			goto err_stop;
		break;
	case FFW_T150:
		ret = tm_t150_probe(tm_wheel);
		if (ret)
			goto err_stop;
		break;
	case FFW_T300:
		dev_info(&hdev->dev, "FFW_T300 Force Feedback features are not yet implemented.\n");
		break;
	case FFW_T500:
		dev_info(&hdev->dev, "FFW_T500 Force Feedback features are not yet implemented.\n");
		break;
	default:
		ret = -ENODEV;
		goto err_stop;
		break;
	}

	return 0;

err_stop:
	hid_hw_stop(hdev);
err_exit:
	return dev_err_probe(&hdev->dev, ret, "Failed to initialize device as Thrustmaster Racing Wheel.\n");
}

static const struct hid_device_id thrustmaster_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_FIRE_DP) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_FIRE_DP2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_DT_2IN1) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_DT_2IN1_PC) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_DT_2IN1_PS3) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_NASCAR_PRO_FF2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_FGT_RUMBLE) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_RGT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_FGT_FFW) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_F430) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_INIT) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_T500RS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_TX) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_T300RS_PS4) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_T300RS_PS3) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_T150RS) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_TMX) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_TS_PC) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_TS_XW) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_T128) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_THRUSTMASTER, USB_DEVICE_ID_TM_T128X) },
	{},
};

MODULE_DEVICE_TABLE(hid, thrustmaster_devices);

static struct hid_driver thrustmaster_driver = {
	.name = "hid-thrustmaster",
	.id_table = thrustmaster_devices,
	.probe = thrustmaster_probe,
	.remove = thrustmaster_remove,
	.report_fixup = thrustmaster_report_fixup,
	.input_mapping	= thrustmaster_input_mapping,
};

module_hid_driver(thrustmaster_driver);

MODULE_AUTHOR("Dario Pagani <dario.pagani.146+linuxk@gmail.com>");
MODULE_AUTHOR("Derek J. Clark <derekjohn.clark@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Driver for Thrustmaster Racing Wheels");
