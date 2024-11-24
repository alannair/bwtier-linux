// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER API
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmail.com>
 */

#include <linux/bwtier_macros.h>

#include <linux/perf_event.h>
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

/* 
 * Ensure that perf ring buffer size is a multiple of this struct's
 * size. Else, the samples at the end of the buffer will be split
 * across buffer's end and start.
 * For simplicity, just ensure that this struct's size is a power of 2.
 */
#define PERF_SAMPLE_TYPE (0ULL | \
		PERF_SAMPLE_IP | \
		PERF_SAMPLE_TID | \
		PERF_SAMPLE_TIME | \
		PERF_SAMPLE_ADDR | \
		PERF_SAMPLE_CPU | \
		PERF_SAMPLE_PERIOD | \
		PERF_SAMPLE_PHYS_ADDR)

struct bwtier_sample {
	struct perf_event_header header; // 8 bytes
	uint64_t ip;
	uint32_t pid, tid;
	uint64_t timestamp;
	uint64_t address;
	uint32_t cpu, res;
	uint64_t period;
	uint64_t phys_addr;
};

struct sys_stat_record {
	uint64_t nr_dram_load_samples;
	uint64_t nr_cxl_load_samples;
	uint64_t nr_dram_store_samples;
	uint64_t nr_cxl_store_samples;
	uint64_t ctr_loads;
	uint64_t ctr_stores;
	uint64_t time_start;
	uint64_t time_dur_ns;
};

struct access_hist_bin {
	int bin_id; // unique ID, starts with 1
	uint32_t nr_dram_pages;
	uint32_t nr_cxl_pages;
	struct list_head dram_pages_head;
	struct list_head cxl_pages_head;
	struct mutex lock;
};

extern struct access_hist_bin *pg_hist_bins;
extern atomic_t oldest_bin_index;
extern atomic_t newest_bin_index;

extern struct cpumask cpu_bitmap;
extern atomic_t stats_ms, migr_ms;
extern atomic_t current_record_index[BWTIER_NR_CPUS + 1];
extern struct sys_stat_record **per_cpu_logs;
extern struct perf_event **eventlist;
extern struct perf_event **counterlist;

void bwtier_msleep(unsigned long msecs);
void cool_once(void);

bool bwtier_status(void);
int bwtier_enable(void);
int bwtier_disable(void);
int ksampld_enable(void);
int ksampld_disable(void);
int kmigrtd_enable(void);
int kmigrtd_disable(void);

int bwtier_migr_ms(void);
int set_bwtier_migr_ms(int ms);
int bwtier_stats_ms(void);
int set_bwtier_stats_ms(int ms);
int max_migr_pages(void);
int set_max_migr_pages(int nrpages);

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

int bin_index_from_bin_id(int bin_id);
int bin_index_from_access_count(int access_count);

void ksampld_init(void);
void kmigrtd_init(void);
void bwtier_core_init(void);
int update_pginfo(struct bwtier_sample *sample, bool is_load);
int bwtier_statistics(char *kbuf, int len);