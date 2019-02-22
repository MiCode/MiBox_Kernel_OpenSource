/*
 * Copyright (C) 2018 XiaoMi, Inc.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/amlogic/aml_gpio_consumer.h>
#include <linux/kthread.h>
//#include <linux/earlysuspend.h>
#include <linux/syscore_ops.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/notifier.h>
#include <linux/cpufreq.h>
#include <linux/mutex.h>

#ifdef CONFIG_AMLOGIC_LEGACY_EARLY_SUSPEND
#include <linux/amlogic/pm.h>
static struct early_suspend powerkey_early_suspend;
#endif


#define SWITCH_LED "switch_led"

#define RN5T567_REG              0x98
#define RN5T567_I2C_ADDRESS      0x32
#define RN5T567_DEV_NAME         "rn5t567"
#define I2C_STATIC_BUS_NUM       0

static int countflag = 1;
static struct class *switch_led_class;
static struct task_struct *ledtask;
static int delaytime;
static int powerkeyflag = -1;
static int poweron_status = 1;
static int pmu_flag;
static int rn5t567_reg;
static struct i2c_client *rn5t567_i2c_client;

static struct mutex ao_i2c_mutex;

/*
 * mutex between dvfs and led control
 */
static int cpu_freq_change_nb(struct notifier_block *nb,
				unsigned long action, void *data)
{
	struct cpufreq_freqs *freqs = data;

	if (freqs->cpu != 0)
		return 0;
	switch (action) {
	case CPUFREQ_PRECHANGE:
		mutex_lock(&ao_i2c_mutex);
		break;

	case CPUFREQ_POSTCHANGE:
		mutex_unlock(&ao_i2c_mutex);
		break;

	default:
		break;
	}
	return 0;
}

/* I2C Read */
/*
 * i2c_smbus_read_byte_data - SMBus "read byte" protocol
 * @client: Handle to slave device
 * @command: Byte interpreted by slave
 *
 * This executes the SMBus "read byte" protocol, returning negative errno
 * else a data byte received from the device.
 */

static int rn5t567_i2c_read_reg(u8 reg)
{
	int ret;

	ret = i2c_smbus_read_byte_data(rn5t567_i2c_client, reg);
	return ret;
}

/* I2C Write */
/*
 * i2c_smbus_write_byte_data - SMBus "write byte" protocol
 * @client: Handle to slave device
 * @command: Byte interpreted by slave
 * @value: Byte being written
 *
 * This executes the SMBus "write byte" protocol, returning negative errno
 * else zero on success.
 */
static int rn5t567_i2c_write_reg(u8 reg, u8 value)
{
	int ret;

	mutex_lock(&ao_i2c_mutex);
	ret = i2c_smbus_write_byte_data(rn5t567_i2c_client, reg, value);
	mutex_unlock(&ao_i2c_mutex);

	if (ret < 0) {
		pr_info("led i2c write fail!!\n");
		return ret;
	} else
		return 0;
}

static struct notifier_block cpu_nb = {
	.notifier_call = cpu_freq_change_nb,
};

static int rn5t567_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	int ret = 0;

	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);

	pr_info("start RN5T567 probe !!\n");

	if (!i2c_check_functionality
	    (adapter,
	     I2C_FUNC_I2C | I2C_FUNC_SMBUS_WRITE_BYTE |
	     I2C_FUNC_SMBUS_READ_BYTE_DATA | I2C_FUNC_SMBUS_I2C_BLOCK)) {
		pr_info(" client is not smb-i2c !!\n");
		ret = -EIO;
		return ret;
	}

	rn5t567_i2c_client = client;

	i2c_set_clientdata(client, rn5t567_i2c_client);

	rn5t567_reg = rn5t567_i2c_read_reg(RN5T567_REG);
	return ret;
}

static int rn5t567_remove(struct i2c_client *client)
{
	if (rn5t567_i2c_client != NULL)
		i2c_unregister_device(rn5t567_i2c_client);
	return 0;
}

static const struct i2c_device_id rn5t567_id[] = {
	{RN5T567_DEV_NAME, 0},
	{}
};

static struct i2c_driver rn5t567_driver = {
	.driver = {
		   .name = RN5T567_DEV_NAME,
		   .owner = THIS_MODULE,
		   },
	.probe = rn5t567_probe,
	.remove = rn5t567_remove,
	.id_table = rn5t567_id,
};

static struct i2c_board_info rn5t567_i2c_boardinfo = {
	I2C_BOARD_INFO(RN5T567_DEV_NAME, RN5T567_I2C_ADDRESS),
};

int i2c_static_add_device(struct i2c_board_info *info)
{
	struct i2c_adapter *adapter;
	struct i2c_client *client;
	int err;

	adapter = i2c_get_adapter(I2C_STATIC_BUS_NUM);
	if (!adapter) {
		pr_err("%s: can't get i2c adapter\n", __func__);
		err = -ENODEV;
		goto i2c_err;
	}

	mutex_init(&ao_i2c_mutex);
	err = cpufreq_register_notifier(&cpu_nb, CPUFREQ_TRANSITION_NOTIFIER);

	client = i2c_new_device(adapter, info);
	if (!client) {
		pr_err("%s:  can't add i2c device at 0x%x\n", __func__,
		       (unsigned int)info->addr);
		err = -ENODEV;
		goto i2c_err;
	}

	i2c_put_adapter(adapter);

	return 0;

i2c_err:
	return err;
}

static void led_en_on(void)
{
	if (pmu_flag)
		rn5t567_i2c_write_reg(RN5T567_REG, 0x30 | rn5t567_reg);

}

static void led_en_off(void)
{
	if (pmu_flag)
		rn5t567_i2c_write_reg(RN5T567_REG, 0xcf & rn5t567_reg);

}

#ifdef CONFIG_AMLOGIC_LEGACY_EARLY_SUSPEND
static void pkey_early_suspend(struct early_suspend *h)
{
	powerkeyflag = 1;
	led_en_off();
}

static void pkey_late_resume(struct early_suspend *h)
{
	powerkeyflag = -1;
	led_en_on();
}
#endif

int ledflash(void *data)
{
	while (countflag != 1) {
		if (countflag % 2 == 0) {
			mdelay(delaytime);
			led_en_on();
		} else {
			mdelay(delaytime);
			led_en_off();
		}

		if (countflag != 1) {
			countflag++;
			if (countflag == 200)
				countflag = 2;
		} else {
			if (kthread_should_stop())
				break;
		}
	}
	return 0;
}

static ssize_t show_switch_led(struct class *class,
			       struct class_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t store_switch_led(struct class *class,
				struct class_attribute *attr, const char *buf,
				size_t count)
{
	if (powerkeyflag == 1)
		return count;

	if (strncmp(buf, "255", 3) == 0) {
		led_en_on();
	} else if (strncmp(buf, "0", 1) == 0) {
		led_en_off();
		countflag = 1;
	} else if (strncmp(buf, "50", 2) == 0) {
		if (countflag == 1) {
			delaytime = 50;
			countflag = 2;
			ledtask = kthread_create(ledflash, NULL, "ledkthread");
			wake_up_process(ledtask);
		}
	} else if (strncmp(buf, "100", 3) == 0) {
		if (countflag == 1) {
			delaytime = 200;
			countflag = 2;
			ledtask = kthread_create(ledflash, NULL, "ledkthread");
			wake_up_process(ledtask);
		}
	} else if (strncmp(buf, "1", 1) == 0) {
		pr_info("screen on and led on\n");
		led_en_on();
	}
	return count;
}

static const struct of_device_id amremote_led_dt_match[] = {
	{
	 .compatible = "amlogic, amremote_led",
	 },
	{},
};

static CLASS_ATTR(buttonslight, 0644, show_switch_led,
		  store_switch_led);

static int led_probe(struct platform_device *pdev)
{
	int ret;
#ifdef CONFIG_AMLOGIC_LEGACY_EARLY_SUSPEND
	powerkey_early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN;
	powerkey_early_suspend.suspend = pkey_early_suspend;
	powerkey_early_suspend.resume = pkey_late_resume;
	register_early_suspend(&powerkey_early_suspend);
#endif

#ifdef CONFIG_OF
	if (pdev->dev.of_node) {
		ret =
		    of_property_read_u32(pdev->dev.of_node, "poweron_status",
					 &poweron_status);
		if (ret < 0) {
			pr_err("led: get poweron_status failed!!\n");
			poweron_status = 1;
		}
		ret =
		    of_property_read_u32(pdev->dev.of_node, "pmu_control",
					 &pmu_flag);
		if (ret < 0) {
			pr_err
			    ("led: get pmu flag failed, only use GPIO mode\n");
			pmu_flag = 0;
		}
	}
#endif
	if (pmu_flag) {
		ret = i2c_static_add_device(&rn5t567_i2c_boardinfo);
		if (ret < 0) {
			pr_err("%s: add rn5t567 i2c device error %d\n",
			       __func__, ret);
			return -EINVAL;
		}
		i2c_add_driver(&rn5t567_driver);
		pr_info("Register rn5t567 i2c driver.\n");

	}

	led_en_on();

	switch_led_class = class_create(THIS_MODULE, SWITCH_LED);
	if (IS_ERR(switch_led_class)) {
		pr_err("create key power class fail\n");
		goto err_create_file;
	}
	if (class_create_file(switch_led_class, &class_attr_buttonslight)) {
		pr_err("create key power file fail\n");
		goto err_create_file;
	}
	return 0;

err_create_file:

	return -EINVAL;
}

static int led_remove(struct platform_device *pdev)
{
	if (pmu_flag) {
		i2c_unregister_device(rn5t567_i2c_client);
		i2c_del_driver(&rn5t567_driver);
	}

	class_remove_file(switch_led_class, &class_attr_buttonslight);
	class_destroy(switch_led_class);
#ifdef CONFIG_AMLOGIC_LEGACY_EARLY_SUSPEND
	unregister_early_suspend(&powerkey_early_suspend);
#endif
	return 0;
}

#ifdef CONFIG_PM
static int led_suspend(void)
{
	led_en_off();
	return 0;
}
#else
#define led_suspend	NULL
#define led_resume	NULL
#endif

struct syscore_ops led_syscore_ops = {
	.suspend = led_suspend,
	.resume = NULL,
};

static struct platform_driver led_driver = {
	.driver = {
		   .name = "amremote_led",
		   .of_match_table = amremote_led_dt_match,
		   .owner = THIS_MODULE,
		   },
	.probe = led_probe,
	.remove = led_remove,
};

static int __init led_init(void)
{
	pr_info("%s, register platform driver...\n", __func__);
	register_syscore_ops(&led_syscore_ops);
	return platform_driver_register(&led_driver);
}

static void __exit led_exit(void)
{
	platform_driver_unregister(&led_driver);
	pr_info("%s, platform driver unregistered ok\n", __func__);
}

module_init(led_init);
module_exit(led_exit);

MODULE_AUTHOR("");
MODULE_DESCRIPTION("Driver for amremote led");
MODULE_LICENSE("GPL");
