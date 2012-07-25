/* linux/arch/arm/mach-exynos/board-manta-battery.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/debugfs.h>

#include <plat/adc.h>
#include <plat/gpio-cfg.h>

#include <linux/platform_data/max17047_fuelgauge.h>
#include <linux/platform_data/bq24191_charger.h>
#include <linux/power/smb347-charger.h>
#include <linux/platform_data/manta_battery.h>

#include "board-manta.h"

#define TA_ADC_LOW		700
#define TA_ADC_HIGH		1750

#define	GPIO_USB_SEL1		EXYNOS5_GPH0(1)
#define	GPIO_POGO_SEL1		EXYNOS5_GPG1(0)
#define	GPIO_TA_EN		EXYNOS5_GPG1(5)
#define	GPIO_TA_INT		EXYNOS5_GPX0(0)
#define	GPIO_TA_nCHG_LUNCHBOX	EXYNOS5_GPG1(4)
#define	GPIO_TA_nCHG_ALPHA	EXYNOS5_GPX0(4)
#define GPIO_OTG_VBUS_SENSE	EXYNOS5_GPX1(0)
#define GPIO_VBUS_POGO_5V	EXYNOS5_GPX1(2)
#define GPIO_OTG_VBUS_SENSE_FAC	EXYNOS5_GPB0(1)

static int gpio_TA_nCHG = GPIO_TA_nCHG_ALPHA;

enum charge_source_type {
	CHARGE_SOURCE_POGO,
	CHARGE_SOURCE_USB,
	CHARGE_SOURCE_MAX,
};

static int cable_type;
static int manta_bat_charge_type[CHARGE_SOURCE_MAX];
static bool manta_bat_usb_online;
static bool manta_bat_pogo_online;
static bool manta_bat_chg_enabled;
static bool manta_bat_chg_enable_synced;
static struct power_supply *manta_bat_smb347_mains;
static struct power_supply *manta_bat_smb347_usb;
static struct power_supply *manta_bat_smb347_battery;

static struct max17047_fg_callbacks *fg_callbacks;
static struct bq24191_chg_callbacks *chg_callbacks;
static struct manta_bat_callbacks *bat_callbacks;

static struct s3c_adc_client *ta_adc_client;

static DEFINE_MUTEX(manta_bat_charger_detect_lock);
static DEFINE_MUTEX(manta_bat_adc_lock);

static void max17047_fg_register_callbacks(struct max17047_fg_callbacks *ptr)
{
	fg_callbacks = ptr;
	if (exynos5_manta_get_revision() >= MANTA_REV_BETA)
		fg_callbacks->get_temperature = NULL;
}

static void max17047_fg_unregister_callbacks(void)
{
	fg_callbacks = NULL;
}

static struct max17047_platform_data max17047_fg_pdata = {
	.register_callbacks = max17047_fg_register_callbacks,
	.unregister_callbacks = max17047_fg_unregister_callbacks,
};

static void bq24191_chg_register_callbacks(struct bq24191_chg_callbacks *ptr)
{
	chg_callbacks = ptr;
}

static void bq24191_chg_unregister_callbacks(void)
{
	chg_callbacks = NULL;
}

static void charger_gpio_init(void)
{
	int hw_rev = exynos5_manta_get_revision();

	gpio_TA_nCHG = hw_rev >= MANTA_REV_PRE_ALPHA ? GPIO_TA_nCHG_ALPHA
		: GPIO_TA_nCHG_LUNCHBOX;

	s3c_gpio_cfgpin(GPIO_TA_INT, S3C_GPIO_INPUT);
	s3c_gpio_setpull(GPIO_TA_INT, S3C_GPIO_PULL_NONE);

	if (hw_rev > MANTA_REV_ALPHA) {
		s3c_gpio_cfgpin(GPIO_OTG_VBUS_SENSE, S3C_GPIO_INPUT);
		s3c_gpio_setpull(GPIO_OTG_VBUS_SENSE, S3C_GPIO_PULL_NONE);

		s3c_gpio_cfgpin(GPIO_VBUS_POGO_5V, S3C_GPIO_INPUT);
		s3c_gpio_setpull(GPIO_VBUS_POGO_5V, S3C_GPIO_PULL_NONE);

		s3c_gpio_cfgpin(GPIO_OTG_VBUS_SENSE_FAC, S3C_GPIO_INPUT);
		s3c_gpio_setpull(GPIO_OTG_VBUS_SENSE_FAC, S3C_GPIO_PULL_NONE);
	}

	s3c_gpio_cfgpin(gpio_TA_nCHG, S3C_GPIO_INPUT);
	s3c_gpio_setpull(gpio_TA_nCHG, hw_rev >= MANTA_REV_PRE_ALPHA ?
			 S3C_GPIO_PULL_NONE : S3C_GPIO_PULL_UP);

	s3c_gpio_cfgpin(GPIO_TA_EN, S3C_GPIO_OUTPUT);
	s3c_gpio_setpull(GPIO_TA_EN, S3C_GPIO_PULL_NONE);
	gpio_set_value(GPIO_TA_EN, 0);

	s3c_gpio_cfgpin(GPIO_USB_SEL1, S3C_GPIO_OUTPUT);
	s3c_gpio_setpull(GPIO_USB_SEL1, S3C_GPIO_PULL_NONE);
	gpio_set_value(GPIO_USB_SEL1, 1);
	s3c_gpio_cfgpin(GPIO_POGO_SEL1, S3C_GPIO_OUTPUT);
	s3c_gpio_setpull(GPIO_POGO_SEL1, S3C_GPIO_PULL_NONE);
	gpio_set_value(GPIO_POGO_SEL1, 1);
}

static int read_ta_adc(enum charge_source_type source)
{
	int vol1, vol2;
	int adc_sel;
	int result;

	mutex_lock(&manta_bat_adc_lock);

	if (source == CHARGE_SOURCE_USB)
		adc_sel = GPIO_USB_SEL1;
	else
		adc_sel = GPIO_POGO_SEL1;

	/* switch to check adc */
	gpio_set_value(adc_sel, 0);
	msleep(100);

	if (exynos5_manta_get_revision() <= MANTA_REV_LUNCHBOX) {
		vol1 = manta_stmpe811_read_adc_data(6);
		vol2 = manta_stmpe811_read_adc_data(6);
	} else {
		vol1 = s3c_adc_read(ta_adc_client, 0);
		vol2 = s3c_adc_read(ta_adc_client, 0);
	}

	if (vol1 >= 0 && vol2 >= 0)
		result = (vol1 + vol2) / 2;
	else
		result = -1;

	msleep(50);

	/* switch back to normal */
	gpio_set_value(adc_sel, 1);
	mutex_unlock(&manta_bat_adc_lock);
	return result;
}

int manta_bat_otg_enable(bool enable)
{
	int ret;
	union power_supply_propval value;

	if (!manta_bat_smb347_usb)
		manta_bat_smb347_usb = power_supply_get_by_name("smb347-usb");

	if (!manta_bat_smb347_usb) {
		pr_err("%s: failed to get smb347-usb power supply\n",
		       __func__);
		return -ENODEV;
	}

	value.intval = enable ? 1 : 0;
	ret = manta_bat_smb347_usb->set_property(manta_bat_smb347_usb,
						 POWER_SUPPLY_PROP_USB_OTG,
						 &value);
	if (ret)
		pr_err("%s: failed to set smb347-usb OTG mode\n",
		       __func__);
	return ret;
}

static bool check_samsung_charger(enum charge_source_type source)
{
	bool result;
	int ta_adc;

	ta_adc = read_ta_adc(source);

	/* ADC range was recommended by HW */
	result = ta_adc > TA_ADC_LOW && ta_adc < TA_ADC_HIGH;
	pr_debug("%s : ta_adc source=%d val=%d, return %d\n", __func__,
		 source, ta_adc, result);
	return result;
}

static int detect_charge_type(enum charge_source_type source, bool online)
{
	int is_ta;
	int charge_type;

	if (!online)
		return CABLE_TYPE_NONE;

	is_ta = check_samsung_charger(source);

	if (is_ta)
		charge_type = CABLE_TYPE_AC;
	else
		charge_type = CABLE_TYPE_USB;

	return charge_type;
}

static int update_charging_status(bool usb_connected, bool pogo_connected)
{
	int i;
	int ret = 0;

	if (manta_bat_usb_online != usb_connected) {
		manta_bat_usb_online = usb_connected;
		manta_bat_charge_type[CHARGE_SOURCE_USB] =
			detect_charge_type(CHARGE_SOURCE_USB, usb_connected);
		ret = 1;
	}

	if (manta_bat_pogo_online != pogo_connected) {
		manta_bat_pogo_online = pogo_connected;
		manta_bat_charge_type[CHARGE_SOURCE_POGO] =
			detect_charge_type(CHARGE_SOURCE_POGO, pogo_connected);
		ret = 1;
	}

	/* Find the highest-priority charging source */
	for (i = 0; i < CHARGE_SOURCE_MAX; i++)
		if (manta_bat_charge_type[i] != CABLE_TYPE_NONE)
			break;

	if (i < CHARGE_SOURCE_MAX)
		cable_type = manta_bat_charge_type[i];
	else
		cable_type = CABLE_TYPE_NONE;

	return ret;
}

static void manta_bat_set_charging_enable(int en)
{
	union power_supply_propval value;

	manta_bat_chg_enabled = en;

	if (exynos5_manta_get_revision() >= MANTA_REV_PRE_ALPHA) {
		if (!manta_bat_smb347_battery)
			manta_bat_smb347_battery =
				power_supply_get_by_name("smb347-battery");

		if (!manta_bat_smb347_battery)
			return;

		value.intval = en ? 1 : 0;
		manta_bat_smb347_battery->set_property(
			manta_bat_smb347_battery,
			POWER_SUPPLY_PROP_CHARGE_ENABLED,
			&value);
		manta_bat_chg_enable_synced = true;

	} else if (chg_callbacks && chg_callbacks->set_charging_enable) {
		chg_callbacks->set_charging_enable(chg_callbacks, en);
	}
}

static void manta_bat_sync_charge_enable(void)
{
	union power_supply_propval chg_enabled = {0,};

	if (!manta_bat_smb347_battery)
		return;

	manta_bat_smb347_battery->get_property(
		manta_bat_smb347_battery,
		POWER_SUPPLY_PROP_CHARGE_ENABLED,
		&chg_enabled);

	if (chg_enabled.intval != manta_bat_chg_enabled) {
		if (manta_bat_chg_enable_synced) {
			pr_info("%s: charger changed enable state to %d\n",
				__func__, chg_enabled.intval);
			manta_bat_chg_enabled = chg_enabled.intval;
		}

		manta_bat_set_charging_enable(manta_bat_chg_enabled);
	}
}

static void change_cable_status(void)
{
	int ta_int;
	union power_supply_propval pogo_connected = {0,};
	union power_supply_propval usb_connected = {0,};
	int status_change = 0;
	int hw_rev = exynos5_manta_get_revision();
	int ret;

	mutex_lock(&manta_bat_charger_detect_lock);
	ta_int = hw_rev <= MANTA_REV_ALPHA ? gpio_get_value(GPIO_TA_INT) :
			gpio_get_value(GPIO_OTG_VBUS_SENSE) |
			gpio_get_value(GPIO_VBUS_POGO_5V);

	if (exynos5_manta_get_revision() >= MANTA_REV_PRE_ALPHA &&
	    (!manta_bat_smb347_mains || !manta_bat_smb347_usb ||
	     !manta_bat_smb347_battery)) {
		manta_bat_smb347_mains =
			power_supply_get_by_name("smb347-mains");
		manta_bat_smb347_usb =
			power_supply_get_by_name("smb347-usb");
		manta_bat_smb347_battery =
			power_supply_get_by_name("smb347-battery");

		if (!manta_bat_smb347_mains || !manta_bat_smb347_usb ||
		    !manta_bat_smb347_battery)
			pr_err("%s: failed to get power supplies\n", __func__);
	}

	if (exynos5_manta_get_revision() > MANTA_REV_ALPHA) {
		if (manta_bat_smb347_usb) {
			usb_connected.intval = gpio_get_value(GPIO_OTG_VBUS_SENSE);
			ret = manta_bat_smb347_usb->set_property(manta_bat_smb347_usb,
					POWER_SUPPLY_PROP_ONLINE,
					&usb_connected);
			if (ret)
				pr_err("%s: failed to change smb347-usb online\n",
					__func__);
		}

		if (manta_bat_smb347_mains) {
			pogo_connected.intval = gpio_get_value(GPIO_VBUS_POGO_5V);
			ret = manta_bat_smb347_mains->set_property(
					manta_bat_smb347_mains,
					POWER_SUPPLY_PROP_ONLINE,
					&pogo_connected);
			if (ret)
				pr_err("%s: failed to change smb347-mains online\n",
					__func__);
		}
	}

	if (ta_int) {
		if (manta_bat_smb347_mains)
			manta_bat_smb347_mains->get_property(
				manta_bat_smb347_mains,
				POWER_SUPPLY_PROP_ONLINE, &pogo_connected);

		if (manta_bat_smb347_usb)
			manta_bat_smb347_usb->get_property(
				manta_bat_smb347_usb,
				POWER_SUPPLY_PROP_ONLINE, &usb_connected);

		if (exynos5_manta_get_revision() >= MANTA_REV_PRE_ALPHA) {
			status_change =
				update_charging_status(usb_connected.intval,
						       pogo_connected.intval);
			manta_bat_sync_charge_enable();
		} else {
			status_change = update_charging_status(true, false);
		}
	} else {
		status_change = update_charging_status(false, false);
	}

	pr_debug("%s: ta_int(%d), cable_type(%d)", __func__,
		 ta_int, cable_type);

	if (status_change && bat_callbacks &&
	    bat_callbacks->change_cable_status)
		bat_callbacks->change_cable_status(bat_callbacks, cable_type);

	mutex_unlock(&manta_bat_charger_detect_lock);
}

static char *exynos5_manta_supplicant[] = { "manta-board" };

static struct smb347_charger_platform_data smb347_chg_pdata = {
	.use_mains = true,
	.use_usb = true,
	.enable_control = SMB347_CHG_ENABLE_PIN_ACTIVE_LOW,
	.usb_mode_pin_ctrl = false,
	.max_charge_current = 2000000,
	.max_charge_voltage = 4200000,
	.pre_charge_current = 200000,
	.termination_current = 150000,
	.pre_to_fast_voltage = 2600000,
	.mains_current_limit = 1800000,
	.usb_hc_current_limit = 1500000,
	.irq_gpio = GPIO_TA_nCHG_ALPHA,
	.en_gpio = GPIO_TA_EN,
	.supplied_to = exynos5_manta_supplicant,
	.num_supplicants = ARRAY_SIZE(exynos5_manta_supplicant),
};

static struct bq24191_platform_data bq24191_chg_pdata = {
	.register_callbacks = bq24191_chg_register_callbacks,
	.unregister_callbacks = bq24191_chg_unregister_callbacks,
	.high_current_charging = 0x06,	/* input current limit 2A */
	.low_current_charging = 0x32,	/* input current linit 500mA */
	.chg_enable = 0x1d,
	.chg_disable = 0x0d,
	.gpio_ta_nchg = GPIO_TA_nCHG_LUNCHBOX,
	.gpio_ta_en = GPIO_TA_EN,
};

static void manta_bat_register_callbacks(struct manta_bat_callbacks *ptr)
{
	bat_callbacks = ptr;
}

static void manta_bat_unregister_callbacks(void)
{
	bat_callbacks = NULL;
}

static int manta_bat_poll_charger_type(void)
{
	change_cable_status();
	return cable_type;
}

static void exynos5_manta_set_mains_current(void)
{
	int ret;
	union power_supply_propval value;

	if (!manta_bat_smb347_mains)
		manta_bat_smb347_mains =
			power_supply_get_by_name("smb347-mains");

	if (!manta_bat_smb347_mains) {
		pr_err("%s: failed to get smb347-mains power supply\n",
		       __func__);
		return;
	}

	value.intval =
		manta_bat_charge_type[CHARGE_SOURCE_POGO] == CHARGER_USB ?
		500000 : 1800000;

	ret = manta_bat_smb347_mains->set_property(manta_bat_smb347_mains,
					 POWER_SUPPLY_PROP_CURRENT_MAX,
					 &value);
	if (ret)
		pr_err("%s: failed to set smb347-mains current limit\n",
		       __func__);
}

static void exynos5_manta_set_usb_hc(void)
{
	int ret;
	union power_supply_propval value;

	if (!manta_bat_smb347_usb)
		manta_bat_smb347_usb = power_supply_get_by_name("smb347-usb");

	if (!manta_bat_smb347_usb) {
		pr_err("%s: failed to get smb347-usb power supply\n",
		       __func__);
		return;
	}

	value.intval =
		manta_bat_charge_type[CHARGE_SOURCE_USB] == CHARGER_USB ?
		0 : 1;
	ret = manta_bat_smb347_usb->set_property(manta_bat_smb347_usb,
						 POWER_SUPPLY_PROP_USB_HC,
						 &value);
	if (ret)
		pr_err("%s: failed to set smb347-usb USB/HC mode\n",
		       __func__);
}

static void manta_bat_set_charging_current(int cable_type)
{
	if (exynos5_manta_get_revision() >= MANTA_REV_PRE_ALPHA) {
		exynos5_manta_set_mains_current();
		exynos5_manta_set_usb_hc();
	} else {
		if (chg_callbacks && chg_callbacks->set_charging_current)
			chg_callbacks->set_charging_current(chg_callbacks,
							    cable_type);
	}
}

static int manta_bat_get_capacity(void)
{
	if (fg_callbacks && fg_callbacks->get_capacity)
		return fg_callbacks->get_capacity(fg_callbacks);
	return -ENXIO;
}

static int manta_bat_get_temperature(int *temp_now)
{
	if (fg_callbacks && fg_callbacks->get_temperature)
		return fg_callbacks->get_temperature(fg_callbacks,
						     temp_now);
	return -ENXIO;
}

static int manta_bat_get_voltage_now(void)
{
	if (fg_callbacks && fg_callbacks->get_voltage_now)
		return fg_callbacks->get_voltage_now(fg_callbacks);
	return -ENXIO;
}

static int manta_bat_get_current_now(int *i_current)
{
	int ret = -ENXIO;

	if (fg_callbacks && fg_callbacks->get_current_now)
		ret = fg_callbacks->get_current_now(fg_callbacks, i_current);
	return ret;
}

static irqreturn_t ta_int_intr(int irq, void *arg)
{
	change_cable_status();
	return IRQ_HANDLED;
}

static struct manta_bat_platform_data manta_battery_pdata = {
	.register_callbacks = manta_bat_register_callbacks,
	.unregister_callbacks = manta_bat_unregister_callbacks,

	.poll_charger_type = manta_bat_poll_charger_type,

	.set_charging_current = manta_bat_set_charging_current,
	.set_charging_enable = manta_bat_set_charging_enable,

	.get_capacity = manta_bat_get_capacity,
	.get_temperature = manta_bat_get_temperature,
	.get_voltage_now = manta_bat_get_voltage_now,
	.get_current_now = manta_bat_get_current_now,

	.temp_high_threshold = 50000,	/* 50c */
	.temp_high_recovery = 42000,	/* 42c */
	.temp_low_recovery = 2000,		/* 2c */
	.temp_low_threshold = 0,		/* 0c */
};

static struct platform_device manta_device_battery = {
	.name = "manta-battery",
	.id = -1,
	.dev.platform_data = &manta_battery_pdata,
};

static struct platform_device *manta_battery_devices[] __initdata = {
	&manta_device_battery,
};

static char *charger_type_str(int cable_type)
{
	switch (cable_type) {
	case CABLE_TYPE_NONE:
		return "none";
	case CABLE_TYPE_AC:
		return "ac";
	case CABLE_TYPE_USB:
		return "usb";
	default:
		break;
	}

	return "?";
}


static int manta_power_debug_dump(struct seq_file *s, void *unused)
{
	if (exynos5_manta_get_revision() > MANTA_REV_ALPHA) {
		seq_printf(s, "ta_en=%d ta_nchg=%d ta_int=%d usbin=%d, dcin=%d\n",
			gpio_get_value(GPIO_TA_EN),
			gpio_get_value(gpio_TA_nCHG),
			gpio_get_value(GPIO_TA_INT),
			gpio_get_value(GPIO_OTG_VBUS_SENSE),
			gpio_get_value(GPIO_VBUS_POGO_5V));
	} else {
		seq_printf(s, "ta_en=%d ta_nchg=%d ta_int=%d\n",
			gpio_get_value(GPIO_TA_EN),
			gpio_get_value(gpio_TA_nCHG),
			gpio_get_value(GPIO_TA_INT));
	}

	seq_printf(s, "usb=%s type=%s; pogo=%s type=%s; selected=%s\n",
		   manta_bat_usb_online ? "online" : "offline",
		   charger_type_str(manta_bat_charge_type[CHARGE_SOURCE_USB]),
		   manta_bat_pogo_online ? "online" : "offline",
		   charger_type_str(manta_bat_charge_type[CHARGE_SOURCE_POGO]),
		   charger_type_str(cable_type));
	return 0;
}

static int manta_power_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, manta_power_debug_dump, inode->i_private);
}

static const struct file_operations manta_power_debug_fops = {
	.open = manta_power_debug_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int manta_power_adc_debug_dump(struct seq_file *s, void *unused)
{
	seq_printf(s, "ta_adc(usb)=%d ta_adc(pogo)=%d\n",
		   read_ta_adc(CHARGE_SOURCE_USB),
		   exynos5_manta_get_revision() >= MANTA_REV_PRE_ALPHA ?
		   read_ta_adc(CHARGE_SOURCE_POGO) : 0);
	return 0;
}

static int manta_power_adc_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, manta_power_adc_debug_dump, inode->i_private);
}

static const struct file_operations manta_power_adc_debug_fops = {
	.open = manta_power_adc_debug_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct i2c_board_info i2c_devs2_common[] __initdata = {
	{
		I2C_BOARD_INFO("max17047-fuelgauge", 0x36),
		.platform_data	= &max17047_fg_pdata,
	},
};

static struct i2c_board_info i2c_devs2_lunchbox[] __initdata = {
	{
		I2C_BOARD_INFO("bq24191-charger", 0x6a),
		.platform_data	= &bq24191_chg_pdata,
	},
};

static struct i2c_board_info i2c_devs2_prealpha[] __initdata = {
	{
		I2C_BOARD_INFO("smb347", 0x0c >> 1),
		.platform_data  = &smb347_chg_pdata,
	},
};

void __init exynos5_manta_battery_init(void)
{
	int hw_rev = exynos5_manta_get_revision();

	charger_gpio_init();

	platform_add_devices(manta_battery_devices,
		ARRAY_SIZE(manta_battery_devices));

	i2c_register_board_info(2, i2c_devs2_common,
				ARRAY_SIZE(i2c_devs2_common));

	if (hw_rev  >= MANTA_REV_PRE_ALPHA)
		i2c_register_board_info(2, i2c_devs2_prealpha,
					ARRAY_SIZE(i2c_devs2_prealpha));
	else
		i2c_register_board_info(2, i2c_devs2_lunchbox,
					ARRAY_SIZE(i2c_devs2_lunchbox));

	if (exynos5_manta_get_revision() >= MANTA_REV_PRE_ALPHA)
		ta_adc_client = s3c_adc_register(&manta_device_battery,
						 NULL, NULL, 0);

	if (IS_ERR_OR_NULL(debugfs_create_file("manta-power", S_IRUGO, NULL,
					       NULL, &manta_power_debug_fops)))
		pr_err("failed to create manta-power debugfs entry\n");

	if (IS_ERR_OR_NULL(debugfs_create_file("manta-power-adc", S_IRUGO, NULL,
					       NULL,
					       &manta_power_adc_debug_fops)))
		pr_err("failed to create manta-power-adc debugfs entry\n");
}

static void exynos5_manta_power_changed(struct power_supply *psy)
{
	change_cable_status();
}

static int exynos5_manta_power_get_property(struct power_supply *psy,
	    enum power_supply_property psp,
	    union power_supply_propval *val)
{
	return -EINVAL;
}

static struct power_supply exynos5_manta_power_supply = {
	.name = "manta-board",
	.type = POWER_SUPPLY_TYPE_UNKNOWN,
	.external_power_changed = exynos5_manta_power_changed,
	.get_property = exynos5_manta_power_get_property,
};

static int __init exynos5_manta_battery_late_init(void)
{
	int ret;
	int hw_rev = exynos5_manta_get_revision();

	ret = power_supply_register(NULL, &exynos5_manta_power_supply);
	if (ret)
		pr_err("%s: failed to register power supply\n", __func__);

	if (hw_rev <= MANTA_REV_ALPHA) {
		ret = request_threaded_irq(gpio_to_irq(GPIO_TA_INT), NULL,
				ta_int_intr,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				IRQF_ONESHOT, "ta_int", NULL);
		if (ret) {
			pr_err("%s: ta_int register failed, ret=%d\n",
					__func__, ret);
		} else {
			ret = enable_irq_wake(gpio_to_irq(GPIO_TA_INT));
			if (ret)
				pr_warn("%s: failed to enable irq_wake for ta_int\n",
					__func__);
		}
	} else {
		ret = request_threaded_irq(gpio_to_irq(GPIO_OTG_VBUS_SENSE),
				NULL, ta_int_intr,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				IRQF_ONESHOT, "usbin_irq", NULL);
		if (ret) {
			pr_err("%s: usbin register failed, ret=%d\n",
				__func__, ret);
		} else {
			ret = enable_irq_wake(gpio_to_irq(GPIO_OTG_VBUS_SENSE));
			if (ret)
				pr_warn("%s: failed to enable irq_wake for usbin\n",
					__func__);
		}

		ret = request_threaded_irq(gpio_to_irq(GPIO_VBUS_POGO_5V), NULL,
				ta_int_intr,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				IRQF_ONESHOT, "usbin_irq", NULL);
		if (ret) {
			pr_err("%s: usbin register failed, ret=%d\n",
					__func__, ret);
		} else {
			ret = enable_irq_wake(gpio_to_irq(GPIO_VBUS_POGO_5V));
			if (ret)
				pr_warn("%s: failed to enable irq_wake for usbin\n",
						__func__);
		}
	}

	/* Poll initial cable state */
	change_cable_status();
	return 0;
}

device_initcall(exynos5_manta_battery_late_init);
