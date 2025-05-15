/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2020-2021 Dario Pagani <dario.pagani.146+linuxk@gmail.com> */
/* Copyright (c) 2020-2021 Kim Kuparinen <kimi.h.kuparinen@gmail.com> */
/* Copyright (c) 2025 Derek J. Clark <derekjohn.clark@gmail.com> */

#include <linux/types.h>

#ifndef _THRUSTMASTER_FFW_TMFF_
#define _THRUSTMASTER_FFW_TMFF_

#define FF_RUMBLE	0x50
#define FF_CONSTANT	0x52
struct hid_device;

enum legacy_ff_effect{
	EFFECT_NONE,
	EFFECT_RUMBLE,
	EFFECT_CONSTANT,
};

static const signed short ff_rumble[] = {
	FF_RUMBLE,
	-1
};

static const signed short ff_constant[] = {
	FF_CONSTANT,
	-1
};

int tmff_init(struct hid_device *hid, enum legacy_ff_effect ff_effect);

#endif // !_THRUSTMASTER_FFW_TMFF_
