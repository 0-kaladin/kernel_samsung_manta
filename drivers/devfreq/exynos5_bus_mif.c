/*
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * EXYNOS - MIF clock frequency scaling support in DEVFREQ framework
 *	This version supports EXYNOS5250 only. This changes bus frequencies.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/io.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/suspend.h>
#include <linux/opp.h>
#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/module.h>
#include <linux/pm_qos.h>

#include <mach/map.h>
#include <mach/regs-clock.h>
#include <mach/asv-exynos.h>
#include <mach/abb-exynos.h>

#include "exynos_ppmu.h"
#include "exynos5_ppmu.h"

#define MAX_SAFEVOLT	1100000 /* 1.1V */

#define MIF_ABB_CONTROL_FREQUENCY 667000000

/* Assume that the bus is saturated if the utilization is 20% */
#define MIF_BUS_SATURATION_RATIO	20

enum mif_level_idx {
	LV_0,
	LV_1,
	LV_2,
	LV_3,
	_LV_END
};

struct busfreq_data_mif {
	struct device *dev;
	struct devfreq *devfreq;
	bool disabled;
	struct regulator *vdd_mif;
	struct opp *curr_opp;

	struct notifier_block pm_notifier;
	struct mutex lock;

	struct clk *mif_clk;
	struct clk *mclk_cdrex;
	struct clk *mout_mpll;
	struct clk *mout_bpll;
};

struct mif_bus_opp_table {
	unsigned int idx;
	unsigned long clk;
	unsigned long volt;
};

static struct mif_bus_opp_table exynos5_mif_opp_table[] = {
	{LV_0, 800000, 1000000},
	{LV_1, 667000, 1000000},
	{LV_2, 400000, 1000000},
	{LV_3, 160000, 1000000},
	{0, 0, 0},
};

static int exynos5_mif_setvolt(struct busfreq_data_mif *data, struct opp *opp)
{
	unsigned long volt;

	rcu_read_lock();
	volt = opp_get_voltage(opp);
	rcu_read_unlock();

	return regulator_set_voltage(data->vdd_mif, volt, MAX_SAFEVOLT);
}

static int exynos5_mif_setclk(struct busfreq_data_mif *data,
		unsigned long new_freq)
{
	unsigned err = 0;
	struct clk *old_p;
	struct clk *new_p;
	unsigned long old_p_rate;
	unsigned long new_p_rate;
	int div;

	/*
	 * Dynamic ABB control according to MIF frequency
	 * MIF frquency > 667 MHz : ABB_MODE_130V
	 * MIF frquency <= 667 MHz : ABB_MODE_BYPASS
	 */
	if (new_freq > MIF_ABB_CONTROL_FREQUENCY)
		set_abb_member(ABB_MIF, ABB_MODE_130V);

	old_p = clk_get_parent(data->mclk_cdrex);
	if (IS_ERR(old_p))
		return PTR_ERR(old_p);
	old_p_rate = clk_get_rate(old_p);
	div = DIV_ROUND_CLOSEST(old_p_rate, new_freq);

	if (abs(DIV_ROUND_UP(old_p_rate, div) - new_freq) > 1000000) {
		new_p = (old_p == data->mout_bpll) ? data->mout_mpll :
				data->mout_bpll;
		new_p_rate = clk_get_rate(new_p);

		if (new_p_rate > old_p_rate) {
			/*
			 * Needs to change to faster pll.  Change the divider
			 * first, then switch to the new pll.  This only works
			 * because the set_rate op on mif_clk doesn't know
			 * anything about its current parent, it just applies
			 * the dividers assuming the right pll has been selected
			 * for the requested frequency.
			 */
			err = clk_set_rate(data->mif_clk, new_freq);
			if (err) {
				pr_info("clk_set_rate error %d\n", err);
				goto out;
			}

			err = clk_set_parent(data->mclk_cdrex, new_p);
			if (err) {
				pr_info("clk_set_parent error %d\n", err);
				goto out;
			}
		} else {
			/*
			 * Needs to change to a slower pll.  Switch to the new
			 * pll first, then apply the new divider.
			 */
			err = clk_set_parent(data->mclk_cdrex, new_p);
			if (err) {
				pr_info("clk_set_parent error %d\n", err);
				goto out;
			}

			err = clk_set_rate(data->mif_clk, new_freq);
			if (err) {
				pr_info("clk_set_rate error %d\n", err);
				goto out;
			}
		}
	} else {
		/* No need to change pll */
		err = clk_set_rate(data->mif_clk, new_freq);
	}

	if (new_freq <= MIF_ABB_CONTROL_FREQUENCY)
		set_abb_member(ABB_MIF, ABB_MODE_BYPASS);
out:
	return err;
}

static int exynos5_busfreq_mif_target(struct device *dev, unsigned long *_freq,
			      u32 flags)
{
	int err = 0;
	struct platform_device *pdev = container_of(dev, struct platform_device,
						    dev);
	struct busfreq_data_mif *data = platform_get_drvdata(pdev);
	struct opp *opp;
	unsigned long old_freq, freq;
	unsigned long volt;

	rcu_read_lock();
	opp = devfreq_recommended_opp(dev, _freq, flags);
	if (IS_ERR(opp)) {
		rcu_read_unlock();
		dev_err(dev, "%s: Invalid OPP.\n", __func__);
		return PTR_ERR(opp);
	}

	freq = opp_get_freq(opp);
	volt = opp_get_voltage(opp);

	old_freq = opp_get_freq(data->curr_opp);
	rcu_read_unlock();

	if (old_freq == freq)
		return 0;

	dev_dbg(dev, "targetting %lukHz %luuV\n", freq, volt);

	mutex_lock(&data->lock);

	if (data->disabled)
		goto out;

	if (old_freq < freq)
		err = exynos5_mif_setvolt(data, opp);
	if (err)
		goto out;

	err = exynos5_mif_setclk(data, freq * 1000);
	if (err)
		goto out;

	if (old_freq > freq)
		err = exynos5_mif_setvolt(data, opp);
	if (err)
		goto out;

	data->curr_opp = opp;
out:
	mutex_unlock(&data->lock);
	return err;
}

static void exynos5_mif_exit(struct device *dev)
{
	struct platform_device *pdev = container_of(dev, struct platform_device,
						    dev);
	struct busfreq_data_mif *data = platform_get_drvdata(pdev);

	devfreq_unregister_opp_notifier(dev, data->devfreq);
}

static struct devfreq_dev_profile exynos5_devfreq_mif_profile = {
	.initial_freq		= 400000,
	.target			= exynos5_busfreq_mif_target,
	.exit			= exynos5_mif_exit,
};

static int exynos5250_init_mif_tables(struct busfreq_data_mif *data)
{
	int i, err = 0;

	for (i = LV_0; i < _LV_END; i++) {
		exynos5_mif_opp_table[i].volt = asv_get_volt(ID_MIF, exynos5_mif_opp_table[i].clk);
		if (exynos5_mif_opp_table[i].volt == 0) {
			dev_err(data->dev, "Invalid value\n");
			return -EINVAL;
		}
	}

	for (i = LV_0; i < _LV_END; i++) {
		err = opp_add(data->dev, exynos5_mif_opp_table[i].clk,
				exynos5_mif_opp_table[i].volt);
		if (err) {
			dev_err(data->dev, "Cannot add opp entries.\n");
			return err;
		}
	}

	return 0;
}

static int exynos5_busfreq_mif_pm_notifier_event(struct notifier_block *this,
		unsigned long event, void *ptr)
{
	struct busfreq_data_mif *data = container_of(this, struct busfreq_data_mif,
						 pm_notifier);
	struct opp *opp;
	unsigned long maxfreq = ULONG_MAX;
	unsigned long freq;
	int err = 0;

	switch (event) {
	case PM_SUSPEND_PREPARE:
		/* Set Fastest and Deactivate DVFS */
		mutex_lock(&data->lock);

		data->disabled = true;

		rcu_read_lock();
		opp = opp_find_freq_floor(data->dev, &maxfreq);
		if (IS_ERR(opp)) {
			rcu_read_unlock();
			err = PTR_ERR(opp);
			goto unlock;
		}
		freq = opp_get_freq(opp);
		rcu_read_unlock();

		err = exynos5_mif_setvolt(data, opp);
		if (err)
			goto unlock;

		err = exynos5_mif_setclk(data, freq * 1000);
		if (err)
			goto unlock;

		data->curr_opp = opp;
unlock:
		mutex_unlock(&data->lock);
		if (err)
			return NOTIFY_BAD;
		return NOTIFY_OK;
	case PM_POST_RESTORE:
	case PM_POST_SUSPEND:
		/* Reactivate */
		mutex_lock(&data->lock);
		data->disabled = false;
		mutex_unlock(&data->lock);
		return NOTIFY_OK;
	}

	return NOTIFY_DONE;
}

static struct devfreq_pm_qos_data exynos5_devfreq_mif_pm_qos_data = {
	.bytes_per_sec_per_hz = 8,
	.pm_qos_class = PM_QOS_MEMORY_THROUGHPUT,
};

static __devinit int exynos5_busfreq_mif_probe(struct platform_device *pdev)
{
	struct busfreq_data_mif *data;
	struct opp *opp;
	struct device *dev = &pdev->dev;
	unsigned long initial_freq;
	int err = 0;

	data = devm_kzalloc(&pdev->dev, sizeof(struct busfreq_data_mif), GFP_KERNEL);
	if (data == NULL) {
		dev_err(dev, "Cannot allocate memory.\n");
		return -ENOMEM;
	}

	data->pm_notifier.notifier_call = exynos5_busfreq_mif_pm_notifier_event;
	data->dev = dev;
	mutex_init(&data->lock);

	err = exynos5250_init_mif_tables(data);
	if (err)
		goto err_regulator;

	data->vdd_mif = regulator_get(dev, "vdd_mif");
	if (IS_ERR(data->vdd_mif)) {
		dev_err(dev, "Cannot get the regulator \"vdd_mif\"\n");
		err = PTR_ERR(data->vdd_mif);
		goto err_regulator;
	}

	data->mif_clk = clk_get(dev, "mif_clk");
	if (IS_ERR(data->mif_clk)) {
		dev_err(dev, "Cannot get clock \"mif_clk\"\n");
		err = PTR_ERR(data->mif_clk);
		goto err_clock;
	}

	data->mclk_cdrex = clk_get(dev, "mclk_cdrex");
	if (IS_ERR(data->mclk_cdrex)) {
		dev_err(dev, "Cannot get clock \"mclk_crex\"\n");
		err = PTR_ERR(data->mclk_cdrex);
		goto err_mclk_cdrex;
	}

	data->mout_mpll = clk_get(dev, "mout_mpll");
	if (IS_ERR(data->mout_mpll)) {
		dev_err(dev, "Cannot get clock \"mout_mpll\"\n");
		err = PTR_ERR(data->mout_mpll);
		goto err_mout_mpll;
	}

	data->mout_bpll = clk_get(dev, "mout_bpll");
	if (IS_ERR(data->mout_bpll)) {
		dev_err(dev, "Cannot get clock \"mout_bpll\"\n");
		err = PTR_ERR(data->mout_bpll);
		goto err_mout_bpll;
	}

	rcu_read_lock();
	opp = opp_find_freq_floor(dev, &exynos5_devfreq_mif_profile.initial_freq);
	if (IS_ERR(opp)) {
		rcu_read_unlock();
		dev_err(dev, "Invalid initial frequency %lu kHz.\n",
		       exynos5_devfreq_mif_profile.initial_freq);
		err = PTR_ERR(opp);
		goto err_opp_add;
	}
	initial_freq = opp_get_freq(opp);
	rcu_read_unlock();

	data->curr_opp = opp;

	err = exynos5_mif_setclk(data, initial_freq * 1000);
	if (err) {
		dev_err(dev, "Failed to set initial frequency\n");
		goto err_opp_add;
	}

	err = exynos5_mif_setvolt(data, opp);
	if (err)
		goto err_opp_add;

	platform_set_drvdata(pdev, data);

	data->devfreq = devfreq_add_device(dev, &exynos5_devfreq_mif_profile,
					   &devfreq_pm_qos,
					   &exynos5_devfreq_mif_pm_qos_data);

	if (IS_ERR(data->devfreq)) {
		err = PTR_ERR(data->devfreq);
		goto err_devfreq_add;
	}

	devfreq_register_opp_notifier(dev, data->devfreq);

	err = register_pm_notifier(&data->pm_notifier);
	if (err) {
		dev_err(dev, "Failed to setup pm notifier\n");
		goto err_devfreq_add;
	}

	return 0;

err_devfreq_add:
	devfreq_remove_device(data->devfreq);
	platform_set_drvdata(pdev, NULL);
err_opp_add:
	clk_put(data->mout_bpll);
err_mout_bpll:
	clk_put(data->mout_mpll);
err_mout_mpll:
	clk_put(data->mclk_cdrex);
err_mclk_cdrex:
	clk_put(data->mif_clk);
err_clock:
	regulator_put(data->vdd_mif);
err_regulator:
	return err;
}

static __devexit int exynos5_busfreq_mif_remove(struct platform_device *pdev)
{
	struct busfreq_data_mif *data = platform_get_drvdata(pdev);

	unregister_pm_notifier(&data->pm_notifier);
	devfreq_remove_device(data->devfreq);
	regulator_put(data->vdd_mif);
	clk_put(data->mif_clk);
	clk_put(data->mclk_cdrex);
	clk_put(data->mout_mpll);
	clk_put(data->mout_bpll);
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver exynos5_busfreq_mif_driver = {
	.probe		= exynos5_busfreq_mif_probe,
	.remove		= __devexit_p(exynos5_busfreq_mif_remove),
	.driver		= {
		.name		= "exynos5-bus-mif",
		.owner		= THIS_MODULE,
	},
};

static int __init exynos5_busfreq_mif_init(void)
{
	return platform_driver_register(&exynos5_busfreq_mif_driver);
}
late_initcall(exynos5_busfreq_mif_init);

static void __exit exynos5_busfreq_mif_exit(void)
{
	platform_driver_unregister(&exynos5_busfreq_mif_driver);
}
module_exit(exynos5_busfreq_mif_exit);
