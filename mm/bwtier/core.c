// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER Core API (PEBS Sampling)
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmail.com>
 */

#include <linux/bwtier.h>
#include <linux/perf_event.h>

#include "../../kernel/events/internal.h"

static bool bwtier_status_var = false;
struct cpumask cpu_bitmap;
struct perf_event **eventlist = NULL;
struct task_struct *ksampld_task = NULL;
static struct hrtimer htimer;
static ktime_t kt_periode;
atomic_t ndram, ncxl, nthrottled, nlost, nzero, nothers;
uint64_t last_head, last_tail;

uint64_t BWTIER_EVENTS[] = { ALL_LOADS_EVENT, ALL_STORES_EVENT };

/* 
 * Ensure that struct size (bytes) is a power of 2.
 * Else we will run into page fault when tail is at the edge 
 * of the buffer, since perf ring buf size is a power of 2.
 */
uint64_t perf_sample_type = 
		PERF_SAMPLE_IP | 
		PERF_SAMPLE_TID |
		// PERF_SAMPLE_TIME | 
		// PERF_SAMPLE_ADDR | 
		PERF_SAMPLE_PHYS_ADDR;

struct bwtier_sample {
	struct perf_event_header header; // 8 bytes
	uint64_t ip;
	uint32_t pid, tid;
	// uint64_t time;
	// uint64_t addr;
	uint64_t phys_addr;
};

static enum hrtimer_restart timer_function(struct hrtimer *timer)
{
	int n_dram = atomic_read(&ndram);
	int n_cxl = atomic_read(&ncxl);
	int n_throttled = atomic_read(&nthrottled);
	int n_lost = atomic_read(&nlost);
	int n_zero = atomic_read(&nzero);
	int n_others = atomic_read(&nothers);

	printk(KERN_INFO 
			"[KSAMPLD] DRAM:%d CXL:%d Thr:%d Lost:%d 0:%d Oth:%d H:%lx T:%lx\n",
			n_dram, n_cxl, n_throttled, n_lost, n_zero, n_others, 
			last_head, last_tail);

	hrtimer_forward_now(timer, kt_periode);
	return HRTIMER_RESTART;
}

static void timer_init(int secs, int nsecs)
{
	atomic_set(&ndram, 0);
	atomic_set(&ncxl, 0);
	atomic_set(&nthrottled, 0);
	atomic_set(&nlost, 0);
	atomic_set(&nzero, 0);
	atomic_set(&nothers, 0);

  kt_periode = ktime_set(secs, nsecs);
  hrtimer_init (&htimer, CLOCK_REALTIME, HRTIMER_MODE_REL);
  htimer.function = timer_function;
  hrtimer_start(&htimer, kt_periode, HRTIMER_MODE_REL);
}

static void timer_cleanup(void)
{
  hrtimer_cancel(&htimer);
}

static int ksampld(void *ksampld_args)
{
	struct perf_buffer *rb;
	struct perf_event_mmap_page *metapage;
	struct perf_event_header *header;
	struct bwtier_sample *sample;
	int event_id, pos, pgshift, nid, iterct, cpu;
	uint64_t data_head, data_tail, pgindex, offset, pfn;

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

					switch (header->type) {
						case PERF_RECORD_SAMPLE:
							sample = (struct bwtier_sample *)header;
							pfn = sample->phys_addr >> PAGE_SHIFT;
							if (!pfn) {
								atomic_inc(&nzero);
								continue;
							}
							nid = pfn_to_nid(pfn);

							if (nid < 2) {
								/* DRAM Address */
								atomic_inc(&ndram);
							} else {
								/* CXL Address */
								atomic_inc(&ncxl);
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

					last_head = data_head;
					last_tail = data_tail + header->size;
				}
			}
		}
	}

	return 0;
}

static int ksampld_start(void)
{
	int pos = 0, ret = 0, cpu, event_id;
	uint32_t nr_pages = 64;

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
						BWTIER_EVENTS[event_id], cpu, PEBS_BUF_PAGES_PER_CPUEVENT);
				if (ret) {
					printk(KERN_ERR "perf_init ret:%d cpu:%d %d\n", ret, cpu, BWTIER_NR_CPUS);
				}
				perf_event_enable(eventlist[pos]);
			}
		}
		printk(KERN_INFO "Outta the loop\n");
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
	timer_init(0, 1e+7);
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
	printk(KERN_INFO "task:%lx evnt:%lx\n", ksampld_task, eventlist);
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

bool bwtier_status(void)
{
	return bwtier_status_var;
}

int bwtier_enable(void)
{
	if (ksampld_task == NULL) {
		ksampld_start();
	}
		
	bwtier_status_var = true;

	return 0;
}

int bwtier_disable(void)
{
	if (ksampld_task != NULL) {
		ksampld_stop();
	}

	bwtier_status_var = false;

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