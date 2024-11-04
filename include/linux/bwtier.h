// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER API
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmail.com>
 */

#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/hrtimer.h>
#include <linux/vmalloc.h>

#define ALL_LOADS_EVENT 0x81d0
#define ALL_STORES_EVENT 0x82d0
#define LLC_MISS_LOADS_EVENT  0x20d1
#define STLB_MISS_STORES_EVENT 0x12d0
#define NUM_BWTIER_EVENTS 2

#define SAMPLE_BATCH_SIZE 100
#define BWTIER_NR_CPUS 96
#define PEBS_BUF_PAGES_PER_CPUEVENT 512

bool bwtier_status(void);
int bwtier_enable(void);
int bwtier_disable(void);
void bwtier_set_cpu_bitmap(struct cpumask *mask);
void bwtier_get_cpu_bitmap(struct cpumask *mask);
void bwtier_enable_all_cpus(void);
void bwtier_disable_all_cpus(void);