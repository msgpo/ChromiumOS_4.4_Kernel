/*
 * cros_ec_dev - expose the Chrome OS Embedded Controller to userspace
 *
 * Copyright (C) 2014 Google, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _CROS_EC_DEV_H_
#define _CROS_EC_DEV_H_

#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/mfd/cros_ec.h>

#define CROS_EC_DEV_VERSION "1.0.0"

#define CROS_EC_DEV_IOC       0xEC
#define CROS_EC_DEV_IOCXCMD   _IOWR(CROS_EC_DEV_IOC, 0, struct cros_ec_command)

/* lightbar utilities */
extern bool ec_has_lightbar(struct cros_ec_dev *ec);
extern int lb_manual_suspend_ctrl(struct cros_ec_dev *ec, uint8_t enable);
extern int lb_suspend(struct cros_ec_dev *ec);
extern int lb_resume(struct cros_ec_dev *ec);

#endif /* _CROS_EC_DEV_H_ */
