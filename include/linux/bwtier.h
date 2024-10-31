// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER API
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmail.com>
 */

#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/hrtimer.h>

#define ALL_LOADS_EVENT 0x81d0
#define ALL_STORES_EVENT 0x82d0
#define NUM_BWTIER_EVENTS 2

#define SAMPLE_BATCH_SIZE 1

bool bwtier_enabled(void);
int bwtier_enable(void);
int bwtier_disable(void);