// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER ksampld (PEBS Sampling Daemon)
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmmonil.com>
 */

#include <linux/bwtier.h>
#include <linux/perf_event.h>

#include "../../kernel/events/internal.h"

struct cpumask cpu_bitmap;
struct perf_event **eventlist = NULL;
struct task_struct *ksampld_task = NULL;

static struct hrtimer htimer;
static ktime_t kt_periode;
atomic_t nall, ndram, ncxl, nthrottled, nlost, nzero, nothers;

atomic_t ksampld_status_var;
atomic_t pages_per_cpuevent;
atomic_t pebsfreq, stats_ms;
atomic_t ntracked_pids;
pid_t *tracked_pids = NULL;

uint64_t BWTIER_EVENTS[] = { 
		LLC_MISS_LOADS_EVENT, 
		STLB_MISS_STORES_EVENT 
};

/* 
 * Ensure that perf ring buffer size is a multiple of this struct's
 * size. Else, the samples at the end of the buffer will be split
 * across buffer's end and start.
 * For simplicity, just ensure that this struct's size is a power of 2.
 */
uint64_t perf_sample_type = 
		PERF_SAMPLE_IP |
		PERF_SAMPLE_TID |
		PERF_SAMPLE_TIME |
		PERF_SAMPLE_ADDR |
		PERF_SAMPLE_CPU |
		PERF_SAMPLE_PERIOD |
		PERF_SAMPLE_PHYS_ADDR;

struct bwtier_sample {
	struct perf_event_header header; // 8 bytes
	uint64_t ip;
	uint32_t pid, tid;
	uint64_t time;
	uint64_t addr;
	uint32_t cpu, res;
	uint64_t period;
	uint64_t phys_addr;
};

static bool pid_is_tracked(pid_t pid)
{
	int i, npids = atomic_read(&ntracked_pids);

	if (npids == 0) {
		return true;
	}

	for (i = 0; i < npids; i++) {
		if (tracked_pids[i] == pid) {
			return true;
		}
	}
	return false;
}

static enum hrtimer_restart timer_function(struct hrtimer *timer)
{
	int n_dram = atomic_read(&ndram);
	int n_cxl = atomic_read(&ncxl);
	int n_throttled = atomic_read(&nthrottled);
	int n_lost = atomic_read(&nlost);
	int n_zero = atomic_read(&nzero);
	int n_others = atomic_read(&nothers);
	int n_all = atomic_read(&nall);

	printk(KERN_INFO "[KSAMPLD] %d DR:%d CX:%d Thr:%d Lost:%d 0:%d Oth:%d\n",
			n_all, n_dram, n_cxl, n_throttled, n_lost, n_zero, n_others);

	hrtimer_forward_now(timer, kt_periode);
	return HRTIMER_RESTART;
}

static void timer_init(void)
{
	int secs, nsecs, stats_period_ms = atomic_read(&stats_ms);

	atomic_set(&ndram, 0);
	atomic_set(&ncxl, 0);
	atomic_set(&nthrottled, 0);
	atomic_set(&nlost, 0);
	atomic_set(&nzero, 0);
	atomic_set(&nothers, 0);
	atomic_set(&nall, 0);

	secs = stats_period_ms / 1000;
	nsecs = (stats_period_ms % 1000) * 1000000;
  kt_periode = ktime_set(secs, nsecs);
  hrtimer_init (&htimer, CLOCK_REALTIME, HRTIMER_MODE_REL);
}

static void timer_start(void)
{
  htimer.function = timer_function;
  hrtimer_start(&htimer, kt_periode, HRTIMER_MODE_REL);
}

static void timer_cleanup(void)
{
  hrtimer_cancel(&htimer);
}

/*
 * TODOs
 * 1. We assume that a sampled entry from PEBS is read immediately
 * 	  by ksampld. Ensure that (data_head - data_tail) <= CONST.
 *    If this gets violated, lower perf period.
 * 2. Control the CPU overhead of ksampld. For this lower both the
 *    perf period and introduce sleep in the ksampld loop.
 * 3. Develop timer into a statistics accumulation and aggregation unit.
 * 4. Sys Interfaces for perf buffer size, frequency, pidfilter, stats period,
 *    cooling period, etc.
 * 5. Check all sysctl limits of perf
 */
static int ksampld(void *ksampld_args)
{
	struct perf_buffer *rb;
	struct perf_event_mmap_page *metapage;
	struct perf_event_header *header;
	struct bwtier_sample *sample;
	struct pginfo *pginfo;
	int event_id, pos, pgshift, iterct, cpu;
	uint64_t data_head, data_tail, pgindex, offset;

	while (!kthread_should_stop()) {
		for (event_id = 0; event_id < NUM_BWTIER_EVENTS; event_id++) {
			cpu = -1;
			while ((cpu = cpumask_next(cpu, &cpu_bitmap)) < BWTIER_NR_CPUS) {
				pos = event_id * BWTIER_NR_CPUS + cpu;
				if (!eventlist[pos]) {
					printk(KERN_ERR "[KSAMPLD] Eventlist[%d] is NULL\n", pos);
					continue;
				}

				__sync_synchronize();
				rb = eventlist[pos]->rb;
				if (!rb) {
					printk(KERN_ERR "[KSAMPLD] Ring Buffer is NULL\n");
					continue;
				}

				metapage = READ_ONCE(rb->user_page);
				data_head = READ_ONCE(metapage->data_head);
				for (iterct = 0; iterct < SAMPLE_BATCH_SIZE; iterct++) {
					data_tail = READ_ONCE(metapage->data_tail);
					if (data_head == data_tail) {
				    break;
					}

					smp_rmb(); /* read barrier */

					pgshift = PAGE_SHIFT + page_order(rb);
					pgindex = (data_tail >> pgshift) & (rb->nr_pages - 1);
					offset = data_tail & ((1 << pgshift) - 1);

					header = (struct perf_event_header *)((char *)
							(rb->data_pages[pgindex]) + offset);
					atomic_inc(&nall);

					switch (header->type) {
						case PERF_RECORD_SAMPLE:
							sample = (struct bwtier_sample *)header;
							if (pid_is_tracked(sample->pid)) {
								pginfo = update_pginfo(sample->pid, sample->addr);
								if (!pginfo) {
									atomic_inc(&nzero);
								} else if (pginfo->nid == 0 || pginfo->nid == 1) {
									/* DRAM Address */
									atomic_inc(&ndram);
								} else if (pginfo->nid == 2 || pginfo->nid == 3) {
									/* CXL Address */
									atomic_inc(&ncxl);
								} else {
									/* Other Address */
									atomic_inc(&nothers);
								}
							} else {
								atomic_inc(&nothers);
							}
							break;

						case PERF_RECORD_THROTTLE:
						case PERF_RECORD_UNTHROTTLE:
							atomic_inc(&nthrottled);
							break;
						case PERF_RECORD_LOST:
							atomic_inc(&nlost);
							break;
						default:
							atomic_inc(&nothers);
							break;
					}

					smp_mb();

					WRITE_ONCE(metapage->data_tail, data_tail + header->size);
				}
			}
		}
	}

	return 0;
}

static int ksampld_start(void)
{
	int pos = 0, ret = 0, cpu, event_id;
	int perfpages = atomic_read(&pages_per_cpuevent);
	int freq = atomic_read(&pebsfreq);

	if (ksampld_task) {
		return 0;
	}

	if (!eventlist) {
		eventlist = vzalloc(sizeof(struct perf_event *) * BWTIER_NR_CPUS * NUM_BWTIER_EVENTS);

		for (event_id = 0; event_id < NUM_BWTIER_EVENTS; event_id++) {
			cpu = -1;
			while ((cpu = cpumask_next(cpu, &cpu_bitmap)) < BWTIER_NR_CPUS) {
				pos = event_id * BWTIER_NR_CPUS + cpu;
				ret = bwtier_perf_event_init(eventlist + pos, perf_sample_type,
						BWTIER_EVENTS[event_id], cpu, perfpages, freq);
				if (ret) {
					printk(KERN_ERR "perf_init ret:%d cpu:%d\n", ret, cpu);
				}
				perf_event_enable(eventlist[pos]);
			}
		}
	} else {
		for (event_id = 0; event_id < NUM_BWTIER_EVENTS; event_id++) {
			cpu = -1;
			while ((cpu = cpumask_next(cpu, &cpu_bitmap)) < BWTIER_NR_CPUS) {
				pos = event_id * BWTIER_NR_CPUS + cpu;
				perf_event_enable(eventlist[pos]);
			}
		}
	}

	msleep(100);
	timer_start();
	ksampld_task = kthread_run(ksampld, NULL, "ksampld");
	return ret;
}

static int ksampld_stop(void)
{
	int event_id, pos, cpu;

	if (!ksampld_task) {
		return 0;
	}

	timer_cleanup();
	kthread_stop(ksampld_task);
	msleep(100);
	ksampld_task = NULL;

	for (event_id = 0; event_id < NUM_BWTIER_EVENTS; event_id++) {
		cpu = -1;
		while ((cpu = cpumask_next(cpu, &cpu_bitmap)) < BWTIER_NR_CPUS) {
			pos = event_id * BWTIER_NR_CPUS + cpu;
			if (eventlist[pos]) {
				perf_event_disable(eventlist[pos]);		
			}
		}
	}

	return 0;
}

void ksampld_init(void)
{
	atomic_set(&ksampld_status_var, 0);
	atomic_set(&pages_per_cpuevent,
		MAX_PAGES_PER_CPUEVENT / 4);
	atomic_set(&pebsfreq, 1000);
	atomic_set(&stats_ms, 100);
	atomic_set(&ntracked_pids, 0);

	tracked_pids = kzalloc(sizeof(pid_t) * MAX_PIDS, GFP_KERNEL);
	timer_init();
}

bool bwtier_status(void)
{
	return atomic_read(&ksampld_status_var);
}

int ksampld_enable(void)
{
	if (ksampld_task == NULL) {
		ksampld_start();
	}
		
	atomic_set(&ksampld_status_var, 1);

	return 0;
}

int ksampld_disable(void)
{
	if (ksampld_task != NULL) {
		ksampld_stop();
	}

	atomic_set(&ksampld_status_var, 0);

	return 0;
}

void bwtier_set_cpu_bitmap(struct cpumask *mask)
{
	cpumask_copy(&cpu_bitmap, mask);
}

void bwtier_get_cpu_bitmap(struct cpumask *mask)
{
	cpumask_copy(mask, &cpu_bitmap);
}

void bwtier_enable_all_cpus(void)
{
	cpumask_setall(&cpu_bitmap);
}

void bwtier_disable_all_cpus(void)
{
	cpumask_clear(&cpu_bitmap);
}

int bwtier_get_pages_per_cpuevent(void)
{
	return atomic_read(&pages_per_cpuevent);
}

int bwtier_set_pages_per_cpuevent(uint32_t pages)
{
	if (!pages || !is_power_of_2(pages) ||
			pages > MAX_PAGES_PER_CPUEVENT)
		return -EINVAL;

	atomic_set(&pages_per_cpuevent, pages);
	return 0;
}

int bwtier_get_pebsfreq(void)
{
	return atomic_read(&pebsfreq);
}

int bwtier_set_pebsfreq(int freq)
{
	int event_id, pos, cpu, ret = 0;

	if (freq < MIN_PEBS_FREQ || freq > MAX_PEBS_FREQ)
		return -EINVAL;

	if (eventlist) {
		for (event_id = 0; event_id < NUM_BWTIER_EVENTS; event_id++) {
			cpu = -1;
			while ((cpu = cpumask_next(cpu, &cpu_bitmap)) < BWTIER_NR_CPUS) {
				pos = event_id * BWTIER_NR_CPUS + cpu;
				if (eventlist[pos]) {
					ret += perf_event_period(eventlist[pos], freq);		
				}
			}
		}
	}

	atomic_set(&pebsfreq, freq);
	return ret;
}

void clear_bwtier_pids(void)
{
	int i;
	for (i = 0; i < MAX_PIDS; i++) {
		if (!tracked_pids[i])
			break;
		tracked_pids[i] = 0;
	}

	atomic_set(&ntracked_pids, 0);
}

pid_t* get_pid_list(int *npids)
{
	*npids = atomic_read(&ntracked_pids);
	return tracked_pids;
}

void bwtier_set_pid(pid_t pid)
{
	int npids = atomic_read(&ntracked_pids);
	if (npids == MAX_PIDS)
		return;

	tracked_pids[npids] = pid;
	atomic_inc(&ntracked_pids);
}

int bwtier_stats_ms(void)
{
	return atomic_read(&stats_ms);
}

int set_bwtier_stats_ms(int ms)
{
	int secs, nsecs;

	atomic_set(&stats_ms, ms);
	secs = ms / 1000;
	nsecs = (ms % 1000) * 1000000;
  kt_periode = ktime_set(secs, nsecs);
	return 0;
}