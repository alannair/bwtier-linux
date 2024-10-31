// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER Core API (PEBS Sampling)
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmail.com>
 */

#include <linux/bwtier.h>
#include <linux/cpumask.h>
#include <linux/perf_event.h>

#include "../../kernel/events/internal.h"

static bool bwtier_enabled_var = false;
struct perf_event **eventlist = NULL;
struct task_struct *ksampld_task = NULL;
static struct hrtimer htimer;
static ktime_t kt_periode;
atomic_t ndram, ncxl, nthrottled, nothers;

uint64_t BWTIER_EVENTS[] = { ALL_LOADS_EVENT, ALL_STORES_EVENT };
uint64_t perf_sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID |
		PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR | PERF_SAMPLE_PHYS_ADDR;
struct bwtier_sample {
	struct perf_event_header header;
	uint64_t ip;
	uint32_t pid, tid;
	uint64_t time;
	uint64_t addr;
	uint64_t phys_addr;
};

static enum hrtimer_restart timer_function(struct hrtimer *timer)
{
	int n_dram = atomic_read(&ndram);
	int n_cxl = atomic_read(&ncxl);
	int n_throttled = atomic_read(&nthrottled);
	int n_others = atomic_read(&nothers);

	printk(KERN_INFO "[KSAMPLD] DRAM:%d CXL:%d Thr:%d Oth:%d\n",
			n_dram, n_cxl, n_throttled, n_others);

	hrtimer_forward_now(timer, kt_periode);
	return HRTIMER_RESTART;
}

static void timer_init(int secs, int nsecs)
{
	atomic_set(&ndram, 0);
	atomic_set(&ncxl, 0);
	atomic_set(&nthrottled, 0);
	atomic_set(&nothers, 0);

  kt_periode = ktime_set(secs, nsecs); //seconds, nanoseconds
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
	int event_id, cpu, pos, pgshift, nid, iterct;
	uint64_t data_head, data_tail, pgindex, offset, pfn;
	int numprintks = 20;

	while (!kthread_should_stop()) {
		for (event_id = 0; event_id < NUM_BWTIER_EVENTS; event_id++) {
			for_each_online_cpu(cpu) {
				pos = event_id * NR_CPUS + cpu;
				if (!eventlist[pos]) {
					continue;
				}

				__sync_synchronize();
				rb = eventlist[pos]->rb;
				if (!rb) {
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

					printk(KERN_INFO "[KSAMPLD] Hdr Type: %d\n", header->type);
					--numprintks;
					if (!(--numprintks)) {
						return 0;
					}

					switch (header->type) {
						case PERF_RECORD_SAMPLE:
							sample = (struct bwtier_sample *)header;
							pfn = sample->phys_addr >> PAGE_SHIFT;
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
	uint32_t nr_pages = 512;

	if (ksampld_task) {
		return 0;
	}

	if (!eventlist) {
		eventlist = kzalloc(sizeof(struct perf_event *) 
				* NR_CPUS * NUM_BWTIER_EVENTS, GFP_KERNEL);

		for (event_id = 0; event_id < NUM_BWTIER_EVENTS; event_id++) {
			for_each_online_cpu(cpu) {
				pos = event_id * NR_CPUS + cpu;
				eventlist[pos] = kzalloc(sizeof(struct perf_event), GFP_KERNEL);
				ret = bwtier_perf_event_init(eventlist[pos], BWTIER_EVENTS[event_id], 
						perf_sample_type, cpu, nr_pages);
				if (ret) {
					printk(KERN_ERR "bwtier_perf_event_open returned %d\n", ret);
				}
				// perf_event_enable(eventlist[pos]);
			}
		}
	} /* else {
		for (event_id = 0; event_id < NUM_BWTIER_EVENTS; event_id++) {
			for_each_online_cpu(cpu) {
				pos = event_id * NR_CPUS + cpu;
				perf_event_enable(eventlist[pos]);
			}
		}
	} */

	timer_init(1, 0);
	ksampld_task = kthread_run(ksampld, NULL, "ksampld");
	return ret;
}

static int ksampld_stop(void)
{
	// int event_id, pos, cpu;

	if (!ksampld_task) {
		return 0;
	}

	kthread_stop(ksampld_task);
	timer_cleanup();
	ksampld_task = NULL;

	// for (event_id = 0; event_id < NUM_BWTIER_EVENTS; event_id++) {
	// 	for_each_online_cpu(cpu) {
	// 		pos = event_id * NR_CPUS + cpu;
	// 		if (eventlist[pos]) {
	// 			perf_event_disable(eventlist[pos]);		
	// 		}
	// 	}		
	// }

	return 0;
}

bool bwtier_enabled(void)
{
	return bwtier_enabled_var;
}

int bwtier_enable(void)
{
	if (ksampld_task == NULL) {
		ksampld_start();
	}
		
	bwtier_enabled_var = true;
	return 0;
}

int bwtier_disable(void)
{
	if (ksampld_task != NULL) {
		ksampld_stop();
	}

	bwtier_enabled_var = false;
	return 0;
}