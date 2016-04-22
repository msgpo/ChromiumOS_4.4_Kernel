/*
 * cros_ec_dev - expose the Chrome OS Embedded Controller to user-space
 *
 * Copyright (C) 2014 Google, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/fs.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_dark_resume.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "cros_ec_dev.h"

/* Device variables */
#define CROS_MAX_DEV 128
static int ec_major;

static const struct attribute_group *cros_ec_groups[] = {
	&cros_ec_attr_group,
	&cros_ec_lightbar_attr_group,
	&cros_ec_vbc_attr_group,
#ifdef CONFIG_MFD_CROS_EC_PD_UPDATE
	&cros_ec_pd_attr_group,
#endif
#ifdef CONFIG_CHARGER_CROS_USB_PD
	&cros_usb_pd_charger_attr_group,
#endif
	NULL,
};

static struct class cros_class = {
	.owner          = THIS_MODULE,
	.name           = "chromeos",
	.dev_groups     = cros_ec_groups,
};

/* Basic communication */
static int ec_get_version(struct cros_ec_dev *ec, char *str, int maxlen)
{
	struct ec_response_get_version *resp;
	static const char * const current_image_name[] = {
		"unknown", "read-only", "read-write", "invalid",
	};
	struct cros_ec_command *msg;
	int ret;

	msg = kmalloc(sizeof(*msg) + sizeof(*resp), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	msg->version = 0;
	msg->command = EC_CMD_GET_VERSION + ec->cmd_offset;
	msg->insize = sizeof(*resp);
	msg->outsize = 0;

	ret = cros_ec_cmd_xfer(ec->ec_dev, msg);
	if (ret < 0)
		goto exit;

	if (msg->result != EC_RES_SUCCESS) {
		snprintf(str, maxlen,
			 "%s\nUnknown EC version: EC returned %d\n",
			 CROS_EC_DEV_VERSION, msg->result);
		ret = -EINVAL;
		goto exit;
	}

	resp = (struct ec_response_get_version *)msg->data;
	if (resp->current_image >= ARRAY_SIZE(current_image_name))
		resp->current_image = 3; /* invalid */

	snprintf(str, maxlen, "%s\n%s\n%s\n%s\n", CROS_EC_DEV_VERSION,
		 resp->version_string_ro, resp->version_string_rw,
		 current_image_name[resp->current_image]);

	ret = 0;
exit:
	kfree(msg);
	return ret;
}

/* Device file ops */
static int ec_device_open(struct inode *inode, struct file *filp)
{
	struct cros_ec_dev *ec = container_of(inode->i_cdev,
					      struct cros_ec_dev, cdev);
	filp->private_data = ec;
	nonseekable_open(inode, filp);
	return 0;
}

static int ec_device_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static ssize_t ec_device_read(struct file *filp, char __user *buffer,
			      size_t length, loff_t *offset)
{
	struct cros_ec_dev *ec = filp->private_data;
	char msg[sizeof(struct ec_response_get_version) +
		 sizeof(CROS_EC_DEV_VERSION)];
	size_t count;
	int ret;

	if (*offset != 0)
		return 0;

	ret = ec_get_version(ec, msg, sizeof(msg));
	if (ret)
		return ret;

	count = min(length, strlen(msg));

	if (copy_to_user(buffer, msg, count))
		return -EFAULT;

	*offset = count;
	return count;
}

/* Ioctls */
static long ec_device_ioctl_xcmd(struct cros_ec_dev *ec, void __user *arg)
{
	long ret;
	struct cros_ec_command u_cmd;
	struct cros_ec_command *s_cmd;

	if (copy_from_user(&u_cmd, arg, sizeof(u_cmd)))
		return -EFAULT;

	s_cmd = kmalloc(sizeof(*s_cmd) + max(u_cmd.outsize, u_cmd.insize),
			GFP_KERNEL);
	if (!s_cmd)
		return -ENOMEM;

	if (copy_from_user(s_cmd, arg, sizeof(*s_cmd) + u_cmd.outsize)) {
		ret = -EFAULT;
		goto exit;
	}

	s_cmd->command += ec->cmd_offset;
	ret = cros_ec_cmd_xfer(ec->ec_dev, s_cmd);
	/* Only copy data to userland if data was received. */
	if (ret < 0)
		goto exit;

	if (copy_to_user(arg, s_cmd, sizeof(*s_cmd) + u_cmd.insize))
		ret = -EFAULT;
exit:
	kfree(s_cmd);
	return ret;
}

static long ec_device_ioctl(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	struct cros_ec_dev *ec = filp->private_data;

	if (_IOC_TYPE(cmd) != CROS_EC_DEV_IOC)
		return -ENOTTY;

	switch (cmd) {
	case CROS_EC_DEV_IOCXCMD:
		return ec_device_ioctl_xcmd(ec, (void __user *)arg);
	}

	return -ENOTTY;
}

/* Module initialization */
static const struct file_operations fops = {
	.open = ec_device_open,
	.release = ec_device_release,
	.read = ec_device_read,
	.unlocked_ioctl = ec_device_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ec_device_ioctl,
#endif
};

static void __remove(struct device *dev)
{
	struct cros_ec_dev *ec = container_of(dev, struct cros_ec_dev,
					      class_dev);
	kfree(ec);
}

static int cros_ec_check_features(struct cros_ec_dev *ec, int feature)
{
	if (ec->features[0] == -1U && ec->features[1] == -1U) {
		/* features bitmap not read yet */
		struct cros_ec_command *msg;
		int ret;

		msg = kzalloc(sizeof(*msg) + sizeof(ec->features), GFP_KERNEL);
		if (!msg)
			return 0;

		msg->command = EC_CMD_GET_FEATURES + ec->cmd_offset;
		msg->insize = sizeof(ec->features);

		ret = cros_ec_cmd_xfer(ec->ec_dev, msg);
		if (ret < 0 || msg->result != EC_RES_SUCCESS) {
			dev_warn(ec->dev, "cannot get EC features: %d/%d\n",
				 ret, msg->result);
			memset(ec->features, 0, sizeof(ec->features));
		} else {
			memcpy(ec->features, msg->data, sizeof(ec->features));
		}
		kfree(msg);
		dev_dbg(ec->dev, "EC features %08x %08x\n",
			ec->features[0], ec->features[1]);
	}

	return ec->features[feature / 32] & EC_FEATURE_MASK_0(feature);
}

static const struct mfd_cell cros_usb_pd_charger_devs[] = {
	{
		.name = "cros-usb-pd-charger",
		.id   = -1,
	},
};

static void cros_ec_usb_pd_charger_register(struct cros_ec_dev *ec)
{
	int ret;

	ret = mfd_add_devices(ec->dev, 0, cros_usb_pd_charger_devs,
			      ARRAY_SIZE(cros_usb_pd_charger_devs),
			      NULL, 0, NULL);
	if (ret)
		dev_err(ec->dev, "failed to add usb-pd-charger device\n");
}

static void cros_ec_sensors_register(struct cros_ec_dev *ec)
{
	/*
	 * Issue a command to get the number of sensor reported.
	 * Build an array of sensors driver and register them all.
	 */
	int ret, i, id, sensor_num;
	struct mfd_cell *sensor_cells;
	struct cros_ec_sensor_platform *sensor_platforms;
	int sensor_type[MOTIONSENSE_TYPE_MAX];
	struct ec_params_motion_sense *params;
	struct ec_response_motion_sense *resp;
	struct cros_ec_command *msg;

	msg = kzalloc(sizeof(struct cros_ec_command) +
		      max(sizeof(*params), sizeof(*resp)), GFP_KERNEL);
	if (msg == NULL)
  		return;

	msg->version = 2;
	msg->command = EC_CMD_MOTION_SENSE_CMD + ec->cmd_offset;
	msg->outsize = sizeof(*params);
	msg->insize = sizeof(*resp);

	params = (struct ec_params_motion_sense *)msg->data;
	params->cmd = MOTIONSENSE_CMD_DUMP;

	ret = cros_ec_cmd_xfer(ec->ec_dev, msg);
	if (ret < 0 || msg->result != EC_RES_SUCCESS) {
		dev_warn(ec->dev, "cannot get EC sensor information: %d/%d\n",
			 ret, msg->result);
		goto error;
	}

	resp = (struct ec_response_motion_sense *)msg->data;
	sensor_num = resp->dump.sensor_count;
	/* Allocate 2 extra sensors in case lid angle or FIFO are needed */
	sensor_cells = kzalloc(sizeof(struct mfd_cell) * (sensor_num + 2),
			       GFP_KERNEL);
	if (sensor_cells == NULL) {
		dev_err(ec->dev, "failed to allocate mfd cells for sensors\n");
		goto error;
	}

	sensor_platforms = kzalloc(sizeof(struct cros_ec_sensor_platform) *
		  (sensor_num + 1), GFP_KERNEL);
	if (sensor_platforms == NULL) {
		dev_err(ec->dev, "failed to prepare sensor command.\n");
		goto error_platforms;
	}

	memset(sensor_type, 0, sizeof(sensor_type));
	id = 0;
	for (i = 0; i < sensor_num; i++) {
		params->cmd = MOTIONSENSE_CMD_INFO;
		params->info.sensor_num = i;
		ret = cros_ec_cmd_xfer(ec->ec_dev, msg);
		if ((ret < 0) || msg->result != EC_RES_SUCCESS) {
			dev_warn(ec->dev, "no info for EC sensor %d : %d/%d\n",
				 i, ret, msg->result);
			continue;
		}
		switch (resp->info.type) {
		case MOTIONSENSE_TYPE_ACCEL:
			sensor_cells[id].name = "cros-ec-accel";
			break;
		case MOTIONSENSE_TYPE_GYRO:
			sensor_cells[id].name = "cros-ec-gyro";
			break;
		case MOTIONSENSE_TYPE_MAG:
			sensor_cells[id].name = "cros-ec-mag";
			break;
		case MOTIONSENSE_TYPE_PROX:
			sensor_cells[id].name = "cros-ec-prox";
			break;
		case MOTIONSENSE_TYPE_LIGHT:
			sensor_cells[id].name = "cros-ec-light";
			break;
		case MOTIONSENSE_TYPE_ACTIVITY:
			sensor_cells[id].name = "cros-ec-activity";
			break;
		default:
			dev_warn(ec->dev, "unknown type %d\n", resp->info.type);
			continue;
		}
		sensor_platforms[id].sensor_num = i;
		sensor_cells[id].id = sensor_type[resp->info.type];
		sensor_cells[id].platform_data = &sensor_platforms[id];
		sensor_cells[id].pdata_size =
			sizeof(struct cros_ec_sensor_platform);

		sensor_type[resp->info.type]++;
		id++;
	}
	if (sensor_type[MOTIONSENSE_TYPE_ACCEL] >= 2) {
		sensor_platforms[id].sensor_num = sensor_num;

		sensor_cells[id].name = "cros-ec-angle";
		sensor_cells[id].id = 0;
		sensor_cells[id].platform_data = &sensor_platforms[id];
		sensor_cells[id].pdata_size =
			sizeof(struct cros_ec_sensor_platform);
		id++;
	}
	if (cros_ec_check_features(ec, EC_FEATURE_MOTION_SENSE_FIFO)) {
		sensor_cells[id].name = "cros-ec-ring";
		id++;
	}

	ret = mfd_add_devices(ec->dev, 0, sensor_cells, id,
			      NULL, 0, NULL);
	if (ret)
		dev_err(ec->dev, "failed to add EC sensors\n");

	kfree(sensor_platforms);
error_platforms:
	kfree(sensor_cells);
error:
	kfree(msg);
}

static int ec_device_probe(struct platform_device *pdev)
{
	int retval = -ENOMEM;
	struct device *dev = &pdev->dev;
	struct cros_ec_dev_platform *ec_platform = dev_get_platdata(dev);
	dev_t devno = MKDEV(ec_major, pdev->id);
	struct cros_ec_dev *ec = kzalloc(sizeof(*ec), GFP_KERNEL);

	if (!ec)
		return retval;

	dev_set_drvdata(dev, ec);
	ec->ec_dev = dev_get_drvdata(dev->parent);
	ec->dev = dev;
	ec->cmd_offset = ec_platform->cmd_offset;
	ec->features[0] = -1U; /* Not cached yet */
	ec->features[1] = -1U; /* Not cached yet */
	device_initialize(&ec->class_dev);
	cdev_init(&ec->cdev, &fops);

	/*
	 * Add the character device
	 * Link cdev to the class device to be sure device is not used
	 * before unbinding it.
	 */
	ec->cdev.kobj.parent = &ec->class_dev.kobj;
	retval = cdev_add(&ec->cdev, devno, 1);
	if (retval) {
		dev_err(dev, ": failed to add character device\n");
		goto cdev_add_failed;
	}

	/*
	 * Add the class device
	 * Link to the character device for creating the /dev entry
	 * in devtmpfs.
	 */
	ec->class_dev.devt = ec->cdev.dev;
	ec->class_dev.class = &cros_class;
	ec->class_dev.parent = dev;
	ec->class_dev.release = __remove;

	retval = dev_set_name(&ec->class_dev, "%s", ec_platform->ec_name);
	if (retval) {
		dev_err(dev, "dev_set_name failed => %d\n", retval);
		goto set_named_failed;
	}

	retval = device_add(&ec->class_dev);
	if (retval) {
		dev_err(dev, "device_register failed => %d\n", retval);
		goto dev_reg_failed;
	}

	/* check whether this EC instance has the PD charge manager */
	if (cros_ec_check_features(ec, EC_FEATURE_USB_PD))
		cros_ec_usb_pd_charger_register(ec);

	/* check whether this EC is a sensor hub. */
	if (cros_ec_check_features(ec, EC_FEATURE_MOTION_SENSE))
		cros_ec_sensors_register(ec);

	/* Take control of the lightbar from the EC. */
	lb_manual_suspend_ctrl(ec, 1);

	dev_dark_resume_add_consumer(dev);

	return 0;

dev_reg_failed:
set_named_failed:
	dev_set_drvdata(dev, NULL);
	cdev_del(&ec->cdev);
cdev_add_failed:
	kfree(ec);
	return retval;
}

static int ec_device_remove(struct platform_device *pdev)
{
	struct cros_ec_dev *ec = dev_get_drvdata(&pdev->dev);
	dev_dark_resume_remove_consumer(&pdev->dev);

	/* Let the EC take over the lightbar again. */
	lb_manual_suspend_ctrl(ec, 0);

	mfd_remove_devices(ec->dev);
	cdev_del(&ec->cdev);
	device_unregister(&ec->class_dev);
	return 0;
}

static const struct platform_device_id cros_ec_id[] = {
	{ "cros-ec-ctl", 0 },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(platform, cros_ec_id);

static int ec_device_suspend(struct device *dev)
{
	struct cros_ec_dev *ec = dev_get_drvdata(dev);
	if (!dev_dark_resume_active(dev))
		lb_suspend(ec);

	return 0;
}

static int ec_device_resume(struct device *dev)
{
	struct cros_ec_dev *ec = dev_get_drvdata(dev);
	char msg[sizeof(struct ec_response_get_version) +
		 sizeof(CROS_EC_DEV_VERSION)];
	int ret;

	/* Be sure the communication with the EC is reestablished */
	ret = ec_get_version(ec, msg, sizeof(msg));
	if (ret < 0) {
		dev_err(ec->ec_dev->dev, "No EC response at resume: %d\n", ret);
		return 0;
	}
	if (!dev_dark_resume_active(dev))
		lb_resume(ec);

	return 0;
}

static const struct dev_pm_ops cros_ec_dev_pm_ops = {
#ifdef CONFIG_PM_SLEEP
	.suspend = ec_device_suspend,
	.resume = ec_device_resume,
#endif
};

static struct platform_driver cros_ec_dev_driver = {
	.driver = {
		.name = "cros-ec-ctl",
		.pm = &cros_ec_dev_pm_ops,
	},
	.probe = ec_device_probe,
	.remove = ec_device_remove,
};

static int __init cros_ec_dev_init(void)
{
	int ret;
	dev_t dev = 0;

	ret  = class_register(&cros_class);
	if (ret) {
		pr_err(CROS_EC_DEV_NAME ": failed to register device class\n");
		return ret;
	}

	/* Get a range of minor numbers (starting with 0) to work with */
	ret = alloc_chrdev_region(&dev, 0, CROS_MAX_DEV, CROS_EC_DEV_NAME);
	if (ret < 0) {
		pr_err(CROS_EC_DEV_NAME ": alloc_chrdev_region() failed\n");
		goto failed_chrdevreg;
	}
	ec_major = MAJOR(dev);

	/* Register the driver */
	ret = platform_driver_register(&cros_ec_dev_driver);
	if (ret < 0) {
		pr_warn(CROS_EC_DEV_NAME ": can't register driver: %d\n", ret);
		goto failed_devreg;
	}
	return 0;

failed_devreg:
	unregister_chrdev_region(MKDEV(ec_major, 0), CROS_MAX_DEV);
failed_chrdevreg:
	class_unregister(&cros_class);
	return ret;
}

static void __exit cros_ec_dev_exit(void)
{
	platform_driver_unregister(&cros_ec_dev_driver);
	unregister_chrdev(ec_major, CROS_EC_DEV_NAME);
	class_unregister(&cros_class);
}

module_init(cros_ec_dev_init);
module_exit(cros_ec_dev_exit);

MODULE_AUTHOR("Bill Richardson <wfrichar@chromium.org>");
MODULE_DESCRIPTION("Userspace interface to the Chrome OS Embedded Controller");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");