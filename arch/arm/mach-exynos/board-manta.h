/*
 * Copyright (C) 2012 Google, Inc.
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef __MACH_EXYNOS_BOARD_MANTA_H
#define __MACH_EXYNOS_BOARD_MANTA_H

#include <mach/irqs.h>

#define MANTA_REV_LUNCHBOX	0x1
#define MANTA_REV_PRE_ALPHA	0x2
#define MANTA_REV_ALPHA		0x3

/* board IRQ allocations */
#define MANTA_IRQ_BOARD_PMIC_START	IRQ_BOARD_START
#define MANTA_IRQ_BOARD_PMIC_NR		16
#define MANTA_IRQ_BOARD_AUDIO_START	(IRQ_BOARD_START + \
					MANTA_IRQ_BOARD_PMIC_NR)
#define MANTA_IRQ_BOARD_AUDIO_NR	27

void exynos5_manta_audio_init(void);
void exynos5_manta_display_init(void);
void exynos5_manta_input_init(void);
void exynos5_manta_power_init(void);
void exynos5_manta_battery_init(void);
void exynos5_manta_wlan_init(void);
void exynos5_manta_media_init(void);
void exynos5_manta_camera_init(void);
void exynos5_manta_sensors_init(void);
void exynos5_manta_gps_init(void);
void exynos5_manta_jack_init(void);
void exynos5_manta_vib_init(void);
void exynos5_manta_nfc_init(void);
void exynos5_manta_bt_init(void);

int exynos5_manta_get_revision(void);
int manta_stmpe811_read_adc_data(u8 channel);
extern int manta_bat_otg_enable(bool enable);

#endif
