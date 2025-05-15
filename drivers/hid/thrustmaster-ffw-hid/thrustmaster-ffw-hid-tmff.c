// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Force feedback support for legacy HID compliant devices by ThrustMaster
 *
 * Copyright (c) 2003 Zinx Verituse <zinx@epicsol.org>
 * Copyright (c) 2002 Johann Deneux
 * Copyright (c) 2025 Derek J. Clark <derekjohn.clark@gmail.com>
 */
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/module.h>

#include "thrustmaster-ffw-hid-tmff.h"

/* Usages for thrustmaster devices I know about */
#define THRUSTMASTER_USAGE_FF (HID_UP_GENDESK | 0xbb)

#define THRUSTMASTER_DEVICE_ID_2_IN_1_DT 0xb320

struct tmff_device {
	struct hid_report *report;
	struct hid_field *ff_field;
};

/* Changes values from 0 to 0xffff into values from minimum to maximum */
static inline int tmff_scale_u16(unsigned int in, int minimum, int maximum)
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
static inline int tmff_scale_s8(int in, int minimum, int maximum)
{
	int ret;

	ret = (((in + 0x80) * (maximum - minimum)) / 0xff) + minimum;
	if (ret < minimum)
		return minimum;
	if (ret > maximum)
		return maximum;
	return ret;
}

static int tmff_play(struct input_dev *dev, void *data,
		struct ff_effect *effect)
{
	struct hid_device *hid = input_get_drvdata(dev);
	struct tmff_device *tmff = data;
	struct hid_field *ff_field = tmff->ff_field;
	int x, y;
	int left, right;	/* Rumbling */

	switch (effect->type) {
	case FF_CONSTANT:
		x = tmff_scale_s8(effect->u.ramp.start_level,
					ff_field->logical_minimum,
					ff_field->logical_maximum);
		y = tmff_scale_s8(effect->u.ramp.end_level,
					ff_field->logical_minimum,
					ff_field->logical_maximum);

		dbg_hid("(x, y)=(%04x, %04x)\n", x, y);
		ff_field->value[0] = x;
		ff_field->value[1] = y;
		hid_hw_request(hid, tmff->report, HID_REQ_SET_REPORT);
		break;

	case FF_RUMBLE:
		left = tmff_scale_u16(effect->u.rumble.weak_magnitude,
					ff_field->logical_minimum,
					ff_field->logical_maximum);
		right = tmff_scale_u16(effect->u.rumble.strong_magnitude,
					ff_field->logical_minimum,
					ff_field->logical_maximum);

		/* 2-in-1 strong motor is left */
		if (hid->product == THRUSTMASTER_DEVICE_ID_2_IN_1_DT)
			swap(left, right);

		dbg_hid("(left,right)=(%08x, %08x)\n", left, right);
		ff_field->value[0] = left;
		ff_field->value[1] = right;
		hid_hw_request(hid, tmff->report, HID_REQ_SET_REPORT);
		break;
	}
	return 0;
}

int tmff_init(struct hid_device *hdev, enum legacy_ff_effect ff_effect)
{
	const unsigned short *ff_bits;
	struct tmff_device *tmff;
	struct hid_report *report;
	struct list_head *report_list;
	struct hid_input *hidinput;
	struct input_dev *input_dev;
	int ret, i;

	switch (ff_effect) {
	case EFFECT_NONE:
		return -EINVAL;
	case EFFECT_RUMBLE:
		ff_bits = ff_rumble;
		break;
	case EFFECT_CONSTANT:
		ff_bits = ff_constant;
		break;
	}

	if (list_empty(&hdev->inputs)) {
		hid_err(hdev, "no inputs found\n");
		return -ENODEV;
	}
	hidinput = list_entry(hdev->inputs.next, struct hid_input, list);
	input_dev = hidinput->input;

	tmff = devm_kzalloc(&hdev->dev, sizeof(struct tmff_device), GFP_KERNEL);
	if (!tmff)
		return -ENOMEM;

	/* Find the report to use */
	report_list = &hdev->report_enum[HID_OUTPUT_REPORT].report_list;
	list_for_each_entry(report, report_list, list) {
		int fieldnum;

		for (fieldnum = 0; fieldnum < report->maxfield; ++fieldnum) {
			struct hid_field *field = report->field[fieldnum];

			if (field->maxusage <= 0)
				continue;

			switch (field->usage[0].hid) {
			case THRUSTMASTER_USAGE_FF:
				if (field->report_count < 2) {
					hid_warn(hdev, "ignoring FF field with report_count < 2\n");
					continue;
				}

				if (field->logical_maximum ==
						field->logical_minimum) {
					hid_warn(hdev, "ignoring FF field with logical_maximum == logical_minimum\n");
					continue;
				}

				if (tmff->report && tmff->report != report) {
					hid_warn(hdev, "ignoring FF field in other report\n");
					continue;
				}

				if (tmff->ff_field && tmff->ff_field != field) {
					hid_warn(hdev, "ignoring duplicate FF field\n");
					continue;
				}

				tmff->report = report;
				tmff->ff_field = field;

				for (i = 0; ff_bits[i] >= 0; i++)
					set_bit(ff_bits[i], input_dev->ffbit);

				break;

			default:
				hid_warn(hdev, "ignoring unknown output usage %08x\n",
					 field->usage[0].hid);
				continue;
			}
		}
	}

	if (!tmff->report) {
		hid_err(hdev, "can't find FF field in output reports\n");
		return -ENODEV;
	}

	ret = input_ff_create_memless(input_dev, tmff, tmff_play);
	if (ret)
		return ret;

	hid_info(hdev, "force feedback for ThrustMaster devices by Zinx Verituse <zinx@epicsol.org>\n");
	return 0;
}



