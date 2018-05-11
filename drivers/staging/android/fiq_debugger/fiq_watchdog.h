/*
 * Copyright (C) 2014 Google, Inc.
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

<<<<<<< HEAD:drivers/staging/android/fiq_debugger/fiq_watchdog.h
#ifndef _FIQ_WATCHDOG_H_
#define _FIQ_WATCHDOG_H_
=======
#ifndef _LINUX_BINDER_H
#define _LINUX_BINDER_H

#include <uapi/linux/android/binder.h>
>>>>>>> 4526c83b32b... BACKPORT: UPSTREAM: ANDROID: binder: remove 32-bit binder interface.:drivers/android/binder.h

void fiq_watchdog_triggered(const struct pt_regs *regs, void *svc_sp);

#endif
