/*
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/errno.h>
#include <linux/cma.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio_event.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/mmc/host.h>
#include <linux/persistent_ram.h>
#include <linux/platform_device.h>
#include <linux/platform_data/exynos_usb3_drd.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/fixed.h>
#include <linux/serial_core.h>
#include <linux/platform_data/stmpe811-adc.h>

#include <asm/mach/arch.h>
#include <asm/hardware/gic.h>
#include <asm/mach-types.h>

#include <plat/adc.h>
#include <plat/clock.h>
#include <plat/cpu.h>
#include <plat/regs-serial.h>
#include <plat/gpio-cfg.h>
#include <plat/devs.h>
#include <plat/iic.h>
#include <plat/sdhci.h>
#include <plat/udc-hs.h>

#include <mach/map.h>
#include <mach/sysmmu.h>
#include <mach/exynos_fiq_debugger.h>
#include <mach/exynos-ion.h>
#include <mach/dwmci.h>

#include "../../../drivers/staging/android/ram_console.h"
#include "board-manta.h"
#include "common.h"
#include "resetreason.h"

static int manta_hw_rev;

static struct gpio manta_hw_rev_gpios[] = {
	{EXYNOS5_GPV1(4), GPIOF_IN, "hw_rev0"},
	{EXYNOS5_GPV1(3), GPIOF_IN, "hw_rev1"},
	{EXYNOS5_GPV1(2), GPIOF_IN, "hw_rev2"},
	{EXYNOS5_GPV1(1), GPIOF_IN, "hw_rev3"},
};

int exynos5_manta_get_revision(void)
{
	return manta_hw_rev;
}

static void manta_init_hw_rev(void)
{
	int ret;
	int i;

	ret = gpio_request_array(manta_hw_rev_gpios,
		ARRAY_SIZE(manta_hw_rev_gpios));

	BUG_ON(ret);

	for (i = 0; i < ARRAY_SIZE(manta_hw_rev_gpios); i++)
		manta_hw_rev |= gpio_get_value(manta_hw_rev_gpios[i].gpio) << i;

	pr_info("Manta HW revision: %d, CPU EXYNOS5250 Rev%d.%d\n",
		manta_hw_rev,
		samsung_rev() >> 4,
		samsung_rev() & 0xf);
}

static struct ram_console_platform_data ramconsole_pdata;

static struct platform_device ramconsole_device = {
	.name           = "ram_console",
	.id             = -1,
	.dev		= {
		.platform_data = &ramconsole_pdata,
	},
};

static struct platform_device persistent_trace_device = {
	.name           = "persistent_trace",
	.id             = -1,
};

static struct resource persistent_clock_resource[] = {
	[0] = DEFINE_RES_MEM(S3C_PA_RTC, SZ_256),
};


static struct platform_device persistent_clock = {
	.name           = "persistent_clock",
	.id             = -1,
	.num_resources	= ARRAY_SIZE(persistent_clock_resource),
	.resource	= persistent_clock_resource,
};

/* Following are default values for UCON, ULCON and UFCON UART registers */
#define MANTA_UCON_DEFAULT	(S3C2410_UCON_TXILEVEL |	\
				 S3C2410_UCON_RXILEVEL |	\
				 S3C2410_UCON_TXIRQMODE |	\
				 S3C2410_UCON_RXIRQMODE |	\
				 S3C2410_UCON_RXFIFO_TOI |	\
				 S3C2443_UCON_RXERR_IRQEN)

#define MANTA_ULCON_DEFAULT	S3C2410_LCON_CS8

#define MANTA_UFCON_DEFAULT	(S3C2410_UFCON_FIFOMODE |	\
				 S5PV210_UFCON_TXTRIG4 |	\
				 S5PV210_UFCON_RXTRIG4)

static struct s3c2410_uartcfg manta_uartcfgs[] __initdata = {
	[0] = {
		.hwport		= 0,
		.flags		= 0,
		.ucon		= MANTA_UCON_DEFAULT,
		.ulcon		= MANTA_ULCON_DEFAULT,
		.ufcon		= MANTA_UFCON_DEFAULT,
	},
	[1] = {
		.hwport		= 1,
		.flags		= 0,
		.ucon		= MANTA_UCON_DEFAULT,
		.ulcon		= MANTA_ULCON_DEFAULT,
		.ufcon		= MANTA_UFCON_DEFAULT,
	},
	/* Do not initialize hwport 2, it will be handled by fiq_debugger */
	[2] = {
		.hwport		= 3,
		.flags		= 0,
		.ucon		= MANTA_UCON_DEFAULT,
		.ulcon		= MANTA_ULCON_DEFAULT,
		.ufcon		= MANTA_UFCON_DEFAULT,
	},
};

static struct gpio_event_direct_entry manta_keypad_key_map[] = {
	{
		.gpio   = EXYNOS5_GPX2(7),
		.code   = KEY_POWER,
	},
	{
		.gpio   = EXYNOS5_GPX2(0),
		.code   = KEY_VOLUMEUP,
	},
	{
		.gpio   = EXYNOS5_GPX2(1),
		.code   = KEY_VOLUMEDOWN,
	}
};

static struct gpio_event_input_info manta_keypad_key_info = {
	.info.func              = gpio_event_input_func,
	.info.no_suspend        = true,
	.debounce_time.tv64	= 5 * NSEC_PER_MSEC,
	.type                   = EV_KEY,
	.keymap                 = manta_keypad_key_map,
	.keymap_size            = ARRAY_SIZE(manta_keypad_key_map)
};

static struct gpio_event_info *manta_keypad_input_info[] = {
	&manta_keypad_key_info.info,
};

static struct gpio_event_platform_data manta_keypad_data = {
	.names  = {
		"manta-keypad",
		NULL,
	},
	.info           = manta_keypad_input_info,
	.info_count     = ARRAY_SIZE(manta_keypad_input_info),
};

static struct platform_device manta_keypad_device = {
	.name   = GPIO_EVENT_DEV_NAME,
	.id     = 0,
	.dev    = {
		.platform_data = &manta_keypad_data,
	},
};

static void __init manta_gpio_power_init(void)
{
	int err = 0;

	err = gpio_request_one(EXYNOS5_GPX2(7), 0, "GPX2(7)");
	if (err) {
		printk(KERN_ERR "failed to request GPX2(7) for "
				"suspend/resume control\n");
		return;
	}
	s3c_gpio_setpull(EXYNOS5_GPX2(7), S3C_GPIO_PULL_NONE);

	gpio_free(EXYNOS5_GPX2(7));
}

static struct stmpe811_callbacks *stmpe811_cbs;
static void stmpe811_register_callback(struct stmpe811_callbacks *cb)
{
	stmpe811_cbs = cb;
}

int manta_stmpe811_read_adc_data(u8 channel)
{
	if (stmpe811_cbs && stmpe811_cbs->get_adc_data)
		return stmpe811_cbs->get_adc_data(channel);

	return -EINVAL;
}

struct stmpe811_platform_data stmpe811_pdata = {
	.register_cb = stmpe811_register_callback,
};

/* ADC */
static struct s3c_adc_platdata manta_adc_data __initdata = {
	.phy_init       = s3c_adc_phy_init,
	.phy_exit       = s3c_adc_phy_exit,
};

/* I2C2 */
static struct i2c_board_info i2c_devs2[] __initdata = {
	{
		I2C_BOARD_INFO("stmpe811-adc", (0x82 >> 1)),
		.platform_data  = &stmpe811_pdata,
	},
};

/* defined in arch/arm/mach-exynos/reserve-mem.c */
extern void exynos_cma_region_reserve(struct cma_region *,
				struct cma_region *, size_t, const char *);

static void __init exynos_reserve_mem(void)
{
	static struct cma_region regions[] = {
		{
			.name = "ion",
			.size = CONFIG_ION_EXYNOS_CONTIGHEAP_SIZE * SZ_1K,
			{
				.alignment = SZ_1M
			}
		},
		{
			.size = 0 /* END OF REGION DEFINITIONS */
		}
	};

	static const char map[] __initconst =
		"ion-exynos=ion;"
		"s5p-mfc-v6/f=fw;"
		"s5p-mfc-v6/a=b1;"
		;

	exynos_cma_region_reserve(regions, NULL, 0, map);
}

static void exynos_dwmci0_cfg_gpio(int width)
{
	unsigned int gpio;

	for (gpio = EXYNOS5_GPC0(0); gpio < EXYNOS5_GPC0(2); gpio++) {
		s3c_gpio_cfgpin(gpio, S3C_GPIO_SFN(2));
		s3c_gpio_setpull(gpio, S3C_GPIO_PULL_NONE);
		s5p_gpio_set_drvstr(gpio, S5P_GPIO_DRVSTR_LV4);
	}

	switch (width) {
	case 8:
		for (gpio = EXYNOS5_GPC1(0); gpio <= EXYNOS5_GPC1(3); gpio++) {
			s3c_gpio_cfgpin(gpio, S3C_GPIO_SFN(2));
			s3c_gpio_setpull(gpio, S3C_GPIO_PULL_NONE);
			s5p_gpio_set_drvstr(gpio, S5P_GPIO_DRVSTR_LV4);
		}
	case 4:
		for (gpio = EXYNOS5_GPC0(3); gpio <= EXYNOS5_GPC0(6); gpio++) {
			s3c_gpio_cfgpin(gpio, S3C_GPIO_SFN(2));
			s3c_gpio_setpull(gpio, S3C_GPIO_PULL_NONE);
			s5p_gpio_set_drvstr(gpio, S5P_GPIO_DRVSTR_LV4);
		}
		break;
	case 1:
		gpio = EXYNOS5_GPC0(3);
		s3c_gpio_cfgpin(gpio, S3C_GPIO_SFN(2));
		s3c_gpio_setpull(gpio, S3C_GPIO_PULL_NONE);
		s5p_gpio_set_drvstr(gpio, S5P_GPIO_DRVSTR_LV4);
	default:
		break;
	}
}

static struct dw_mci_board exynos_dwmci0_pdata __initdata = {
	.num_slots		= 1,
	.quirks			= DW_MCI_QUIRK_BROKEN_CARD_DETECTION |
				  DW_MCI_QUIRK_HIGHSPEED,
	.bus_hz			= 100 * 1000 * 1000,
	.max_bus_hz		= 100 * 1000 * 1000,
	.caps			= MMC_CAP_UHS_DDR50 | MMC_CAP_1_8V_DDR |
				  MMC_CAP_8_BIT_DATA | MMC_CAP_CMD23,
	.fifo_depth             = 0x80,
	.detect_delay_ms	= 200,
	.hclk_name		= "dwmci",
	.cclk_name		= "sclk_dwmci",
	.cfg_gpio		= exynos_dwmci0_cfg_gpio,
	.sdr_timing		= 0x03020001,
	.ddr_timing		= 0x03030002,
};

static struct platform_device *manta_devices[] __initdata = {
	&ramconsole_device,
	&persistent_trace_device,
	&persistent_clock,
	&s3c_device_i2c1,
	&s3c_device_i2c2,
	&s3c_device_i2c3,
	&s3c_device_i2c4,
	&s3c_device_i2c5,
	&s3c_device_i2c7,
	&s3c_device_adc,
	&s3c_device_wdt,
	&manta_keypad_device,
	&exynos5_device_dwmci0,
	&exynos_device_ion,
	&s3c_device_usb_hsotg,
};

static struct s3c_hsotg_plat manta_hsotg_pdata;

static void __init manta_udc_init(void)
{
	struct s3c_hsotg_plat *pdata = &manta_hsotg_pdata;

	s3c_hsotg_set_platdata(pdata);

	gpio_request_one(EXYNOS5_GPH0(1), GPIOF_INIT_HIGH, "usb_sel");
}

static void __init manta_dwmci_init(void)
{
	exynos_dwmci_set_platdata(&exynos_dwmci0_pdata, 0);
	dev_set_name(&exynos5_device_dwmci0.dev, "exynos4-sdhci.0");
	clk_add_alias("dwmci", "dw_mmc.0", "hsmmc", &exynos5_device_dwmci0.dev);
	clk_add_alias("sclk_dwmci", "dw_mmc.0", "sclk_mmc",
		      &exynos5_device_dwmci0.dev);
}

static void __init manta_map_io(void)
{
	clk_xusbxti.rate = 24000000;
	clk_xxti.rate = 24000000;
	exynos_init_io(NULL, 0);
	s3c24xx_init_clocks(clk_xusbxti.rate);
	s3c24xx_init_uarts(manta_uartcfgs, ARRAY_SIZE(manta_uartcfgs));
}

static void __init manta_sysmmu_init(void)
{
}

static struct persistent_ram_descriptor manta_prd[] __initdata = {
	{
		.name = "ram_console",
		.size = SZ_2M,
	},
#ifdef CONFIG_PERSISTENT_TRACER
	{
		.name = "persistent_trace",
		.size = SZ_1M,
	},
#endif
};

static struct persistent_ram manta_pr __initdata = {
	.descs = manta_prd,
	.num_descs = ARRAY_SIZE(manta_prd),
	.start = PLAT_PHYS_OFFSET + SZ_1G + SZ_512M,
#ifdef CONFIG_PERSISTENT_TRACER
	.size = 3 * SZ_1M,
#else
	.size = SZ_2M,
#endif
};

static void __init manta_init_early(void)
{
	persistent_ram_early_init(&manta_pr);
}

static void __init manta_machine_init(void)
{
	manta_init_hw_rev();
	exynos_serial_debug_init(2, 0);

	manta_sysmmu_init();
	exynos_ion_set_platdata();
	manta_dwmci_init();

	s3c_i2c1_set_platdata(NULL);
	s3c_i2c2_set_platdata(NULL);
	s3c_i2c3_set_platdata(NULL);
	s3c_i2c4_set_platdata(NULL);
	s3c_i2c5_set_platdata(NULL);
	s3c_i2c7_set_platdata(NULL);

	if (exynos5_manta_get_revision() <= MANTA_REV_LUNCHBOX)
		i2c_register_board_info(2, i2c_devs2, ARRAY_SIZE(i2c_devs2));
	else
		s3c_adc_set_platdata(&manta_adc_data);

	manta_gpio_power_init();

	manta_udc_init();
	ramconsole_pdata.bootinfo = exynos_get_resetreason();
	platform_add_devices(manta_devices, ARRAY_SIZE(manta_devices));

	exynos5_manta_power_init();
	exynos5_manta_display_init();
	exynos5_manta_input_init();
	exynos5_manta_battery_init();
	exynos5_manta_wlan_init();
	exynos5_manta_audio_init();
	exynos5_manta_media_init();
	exynos5_manta_camera_init();
	exynos5_manta_sensors_init();
	exynos5_manta_gps_init();
	exynos5_manta_jack_init();

}

MACHINE_START(MANTA, "Manta")
	.atag_offset	= 0x100,
	.init_early	= manta_init_early,
	.init_irq	= exynos5_init_irq,
	.map_io		= manta_map_io,
	.handle_irq	= gic_handle_irq,
	.init_machine	= manta_machine_init,
	.timer		= &exynos4_timer,
	.restart	= exynos5_restart,
	.reserve	= exynos_reserve_mem,
MACHINE_END
