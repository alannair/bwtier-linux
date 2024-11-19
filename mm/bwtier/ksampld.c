// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER ksampld (PEBS Sampling Daemon)
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmmonil.com>
 */

#include <linux/bwtier.h>

#include "../../kernel/events/internal.h"

struct cpumask cpu_bitmap;
struct perf_event **eventlist = NULL;
struct perf_event **counterlist = NULL;
struct task_struct *ksampld_task = NULL;

atomic_t pages_per_cpuevent;
atomic_t pebsfreq;
atomic_t ntracked_pids;
pid_t *tracked_pids = NULL;

const uint64_t BWTIER_EVENTS[] = { 
		LLC_MISS_LOADS_EVENT, 
		STLB_MISS_STORES_EVENT 
};
const uint64_t BWTIER_COUNTERS[] = {
		LLC_MISS_LOADS_COUNTER,
		LLC_MISS_STORES_COUNTER
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

static int ksampld(void *ksampld_args)
{
	struct perf_buffer *rb;
	struct perf_event_mmap_page *metapage;
	struct perf_event_header *header;
	struct bwtier_sample *sample;
	int event_id, pos, pgshift, iterct, cpu;
	uint64_t data_head, data_tail, pgindex, offset;
	uint64_t now = ktime_get_ns(), last = now, tdelta_ms;
	int sample_batchsz = atomic_read(&pebsfreq) * (1000 / KSAMPLD_PERIOD_MS);

	while (!kthread_should_stop()) {
		now = ktime_get_ns();

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
				for (iterct = 0; iterct < sample_batchsz; iterct++) {
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
							if (pid_is_tracked(sample->pid)) {
								update_pginfo(sample, !(event_id % 2));
							}
							break;
						default:
							break;
					}

					smp_mb();

					WRITE_ONCE(metapage->data_tail, data_tail + header->size);
				}
			}
		}

		last = now;
		now = ktime_get_ns();
		tdelta_ms = (now - last) / NSEC_PER_MSEC;
		bwtier_msleep(KSAMPLD_PERIOD_MS - tdelta_ms);
	}

	return 0;
}

static int ksampld_start(void)
{
	int pos = 0, cpu, event_id, ret;
	int perfpages = atomic_read(&pages_per_cpuevent);
	int freq = atomic_read(&pebsfreq);

	if (ksampld_task != NULL)
		return -EBUSY;

	if (!eventlist) {
		eventlist = vzalloc(sizeof(struct perf_event *) *
				BWTIER_NR_CPUS * NUM_BWTIER_EVENTS);

		for (event_id = 0; event_id < NUM_BWTIER_EVENTS; event_id++) {
			cpu = -1;
			while ((cpu = cpumask_next(cpu, &cpu_bitmap)) < BWTIER_NR_CPUS) {
				pos = event_id * BWTIER_NR_CPUS + cpu;
				ret = bwtier_perf_event_init(eventlist + pos, PERF_SAMPLE_TYPE,
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
				if (eventlist[pos])
					perf_event_enable(eventlist[pos]);
			}
		}
	}

	if (!counterlist) {
		counterlist = vzalloc(sizeof(struct perf_event *) *
				BWTIER_NR_CPUS * NUM_BWTIER_COUNTERS);

		for (event_id = 0; event_id < NUM_BWTIER_COUNTERS; event_id++) {
			cpu = -1;
			while ((cpu = cpumask_next(cpu, &cpu_bitmap)) < BWTIER_NR_CPUS) {
				pos = event_id * BWTIER_NR_CPUS + cpu;
				ret = bwtier_perf_counter_init(counterlist + pos, 
						BWTIER_COUNTERS[event_id], cpu);
				if (ret) {
					printk(KERN_ERR "perf_ctr_init ret:%d cpu:%d\n", ret, cpu);
				}
			}
		}
	} else {
		for (event_id = 0; event_id < NUM_BWTIER_COUNTERS; event_id++) {
			cpu = -1;
			while ((cpu = cpumask_next(cpu, &cpu_bitmap)) < BWTIER_NR_CPUS) {
				pos = event_id * BWTIER_NR_CPUS + cpu;
				if (counterlist[pos])
					perf_event_enable(counterlist[pos]);
			}
		}
	}

	msleep(100);
	ksampld_task = kthread_run(ksampld, NULL, "ksampld");
	return 0;
}

static int ksampld_stop(void)
{
	int pos = 0, cpu, event_id;

	if (!ksampld_task) {
		return 0;
	}

	kthread_stop(ksampld_task);
	ksampld_task = NULL;

	for (event_id = 0; event_id < NUM_BWTIER_EVENTS; event_id++) {
		cpu = -1;
		while ((cpu = cpumask_next(cpu, &cpu_bitmap)) < BWTIER_NR_CPUS) {
			pos = event_id * BWTIER_NR_CPUS + cpu;
			if (eventlist[pos])
				perf_event_disable(eventlist[pos]);		
		}
	}

	for (event_id = 0; event_id < NUM_BWTIER_COUNTERS; event_id++) {
		cpu = -1;
		while ((cpu = cpumask_next(cpu, &cpu_bitmap)) < BWTIER_NR_CPUS) {
			pos = event_id * BWTIER_NR_CPUS + cpu;
			if (counterlist[pos])
				perf_event_disable(counterlist[pos]);
		}
	}

	return 0;
}

void ksampld_init(void)
{
	int perfpages = MAX_PAGES_PER_CPUEVENT / 4;
	int freq = 1000;

	atomic_set(&pages_per_cpuevent, perfpages);
	atomic_set(&pebsfreq, freq);
	atomic_set(&ntracked_pids, 0);

	bwtier_enable_all_cpus();
	tracked_pids = kzalloc(sizeof(pid_t) * MAX_PIDS, GFP_KERNEL | __GFP_NOWARN);
}

int ksampld_enable(void)
{
	if (ksampld_task == NULL) {
		ksampld_start();
	}

	return 0;
}

int ksampld_disable(void)
{
	if (ksampld_task != NULL) {
		ksampld_stop();
	}

	return 0;
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
	if (npids == MAX_PIDS || pid <= 0)
		return;

	tracked_pids[npids] = pid;
	atomic_inc(&ntracked_pids);
}
