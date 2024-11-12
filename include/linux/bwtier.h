// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER API
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmail.com>
 */

#include <linux/bwtier_macros.h>

#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/hrtimer.h>
#include <linux/vmalloc.h>

struct access_hist_bin {
	int bin_id; // unique ID, starts with 1
	uint32_t nr_pages;
	struct list_head pages_head;
};

/* Information about sampled page to be returned to ksampld
 * for updating various counters and statistics.
 */
struct pginfo {
	uint32_t bin_id;
	uint32_t nid;
};

bool bwtier_status(void);
int bwtier_enable(void);
int bwtier_disable(void);
int ksampld_enable(void);
int ksampld_disable(void);
int bwtier_cool_ms(void);
int set_bwtier_cool_ms(int ms);
int bwtier_stats_ms(void);
int set_bwtier_stats_ms(int ms);
void clear_bwtier_pids(void);
pid_t *get_pid_list(int *npids);
void bwtier_set_pid(pid_t pid);
int bwtier_get_pebsfreq(void);
int bwtier_set_pebsfreq(int freq);
int bwtier_get_pages_per_cpuevent(void);
int bwtier_set_pages_per_cpuevent(uint32_t pages);

void bwtier_set_cpu_bitmap(struct cpumask *mask);
void bwtier_get_cpu_bitmap(struct cpumask *mask);
void bwtier_enable_all_cpus(void);
void bwtier_disable_all_cpus(void);

void ksampld_init(void);
void bwtier_core_init(void);
struct pginfo* update_pginfo(pid_t pid, uint64_t vaddr);