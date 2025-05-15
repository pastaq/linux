/* SPDX-License-Identifier: GPL-2.0 */

/* Copyright (c) 2020-2021 Dario Pagani <dario.pagani.146+linuxk@gmail.com> */
/* Copyright (c) 2020-2021 Kim Kuparinen <kimi.h.kuparinen@gmail.com> */
/* Copyright (c) 2025 Derek J. Clark <derekjohn.clark@gmail.com> */
#include <linux/types.h>

#ifndef _THRUSTMASTER_FFW_INIT_
#define _THRUSTMASTER_FFW_INIT_

struct tmffw_drvdata;

/*
 * This structs contains (in little endian) the response data
 * of the wheel to the request 73
 *
 * A sufficient research to understand what each field does is not
 * beign conducted yet. The position and meaning of fields are a
 * just a very optimistic guess based on instinct....
 */
struct __packed tmff_urb_response
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

int tmffw_init_probe(struct tmffw_drvdata *tm_wheel);
#endif // !_THRUSTMASTER_FFW_INIT_
