/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2020-2021 Dario Pagani <dario.pagani.146+linuxk@gmail.com> */
/* Copyright (c) 2020-2021 Kim Kuparinen <kimi.h.kuparinen@gmail.com> */
/* Copyright (c) 2025 Derek J. Clark <derekjohn.clark@gmail.com> */

#ifndef _THRUSTMASTER_FFW_CORE_
#define _THRUSTMASTER_FFW_CORE_

enum tmffw_if_type {
	FFW_LEGACY,
	FFW_SETUP,
	FFW_T150,
	FFW_T300,
	FFW_T500,
	FFW_UNKNOWN,
};

struct tmffw_drvdata {
	struct usb_ctrlrequest *change_request;
	struct usb_ctrlrequest *model_request;
	struct tmff_urb_response *response;
	struct usb_device *usb_dev;
	struct hid_device *hdev;
};

#endif // !_THRUSTMASTER_FFW_CORE_
