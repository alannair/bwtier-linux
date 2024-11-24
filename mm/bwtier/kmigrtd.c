// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER kmigrtd (PEBS Migration Daemon)
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmail.com>
 */

#include <linux/bwtier.h>
#include <linux/migrate.h>

struct sys_stat_record** per_cpu_logs;
atomic_t current_record_index[BWTIER_NR_CPUS + 1]; // last is for total
struct task_struct *kmigrtd_task = NULL;

atomic_t stats_ms, migr_ms;
atomic_t max_migr_pages_per_round;

static struct folio *alloc_target_page(struct folio *src, unsigned long node)
{
	struct page *page, *newpage;
	int bin_index;
	gfp_t gfp_mask = (GFP_HIGHUSER_MOVABLE | __GFP_THISNODE |
			__GFP_NOMEMALLOC | __GFP_NORETRY | __GFP_NOWARN) &
			~__GFP_RECLAIM;

	if (folio_test_hugetlb(src) || folio_test_pmd_mappable(src))
		return NULL;

	page = folio_page(src, 0);
	newpage = __alloc_pages_node((int)node, gfp_mask, 0);
	bin_index = bin_index_from_bin_id(page->bin_id);

	mutex_lock(&(pg_hist_bins[bin_index].lock));
	list_del(&page->bwtier_list);
	if (node == 2 || node == 3) {
		// migrating DRAM -> CXL
		list_add(&(newpage->bwtier_list),
			  &(pg_hist_bins[bin_index].cxl_pages_head));
	} else {
		// migrating CXL -> DRAM
		list_add(&(newpage->bwtier_list),
			  &(pg_hist_bins[bin_index].dram_pages_head));
	}
	mutex_unlock(&(pg_hist_bins[bin_index].lock));

	newpage->bin_id = page->bin_id;
	newpage->access_count = page->access_count;
	newpage->last_cooled_timestamp = page->last_cooled_timestamp;

	page->bin_id = 0;
	page->access_count = 0;
	page->last_cooled_timestamp = 0;

	return page_folio(newpage);
}

static int bw_balance(void)
{
	int max_migr_pages = atomic_read(&max_migr_pages_per_round);
	int old_index = atomic_read(&oldest_bin_index);
	int new_index = atomic_read(&newest_bin_index);
	int nr_pages_migrated = 0, nrsuccess = 0;
	unsigned long targetnid = 2;

	while (new_index != old_index && 
			nr_pages_migrated < max_migr_pages) {
		// mutex_lock(&(pg_hist_bins[new_index].lock));
		migrate_pages(&(pg_hist_bins[new_index].dram_pages_head),
				alloc_target_page, NULL, targetnid, MIGRATE_ASYNC,
				MR_BWTIER, &nrsuccess);
		// mutex_unlock(&(pg_hist_bins[new_index].lock));

		pg_hist_bins[new_index].nr_dram_pages -= nrsuccess;
		pg_hist_bins[new_index].nr_cxl_pages += nrsuccess;

		targetnid = (targetnid == 2) ? 3 : 2;
		nr_pages_migrated += nrsuccess;
		new_index = (new_index + NUM_BWTIER_BINS - 1) % NUM_BWTIER_BINS;
	}

	printk(KERN_INFO "Migrated %d pages\n", nr_pages_migrated);

	return nr_pages_migrated;
}

static int kmigrtd(void *kmigrtd_args)
{
	uint64_t start, loads, stores, enabled, running;
	uint64_t sumloads = 0, sumstores = 0;
	uint64_t ncxl_ld = 0, ncxl_st = 0, ndram_ld = 0, ndram_st = 0;
	uint64_t now = ktime_get_ns(), last = now, tdelta_ms, ms_since_migr = 0;
	int stats_aggr_ms = atomic_read(&stats_ms);
	int migr_period_ms = atomic_read(&migr_ms);
	int cpu = -1, index;

	while (!kthread_should_stop()) {
		now = ktime_get_ns();

		if (ms_since_migr >= migr_period_ms) {
			ms_since_migr -= migr_period_ms;
			if (max_migr_pages() > 0)
				bw_balance();
			cool_once();
		}

		cpu = -1;
		while ((cpu = cpumask_next(cpu, &cpu_bitmap)) < BWTIER_NR_CPUS) {
			index = atomic_read(&current_record_index[cpu]);

			start = per_cpu_logs[cpu][index].time_start;
			per_cpu_logs[cpu][index].time_dur_ns = now - start;

			loads = bwtier_perf_counter_read(counterlist[cpu], &enabled, &running);
			per_cpu_logs[cpu][index].ctr_loads = loads;
			stores = bwtier_perf_counter_read(counterlist[cpu + BWTIER_NR_CPUS], 
					&enabled, &running);
			per_cpu_logs[cpu][index].ctr_stores = stores;

			sumloads += loads;
			sumstores += stores;
			ndram_ld += per_cpu_logs[cpu][index].nr_dram_load_samples;
			ncxl_ld += per_cpu_logs[cpu][index].nr_cxl_load_samples;
			ndram_st += per_cpu_logs[cpu][index].nr_dram_store_samples;
			ncxl_st += per_cpu_logs[cpu][index].nr_cxl_store_samples;

			index = (index + 1) % SYS_RECORDS_HIST_LEN;
			memset(&per_cpu_logs[cpu][index], 0, sizeof(struct sys_stat_record));
			atomic_set(&current_record_index[cpu], index);
			per_cpu_logs[cpu][index].time_start = now;
		}

		// totals
		index = atomic_read(&current_record_index[BWTIER_NR_CPUS]);
		start = per_cpu_logs[BWTIER_NR_CPUS][index].time_start;
		per_cpu_logs[BWTIER_NR_CPUS][index].time_dur_ns = now - start;
		per_cpu_logs[BWTIER_NR_CPUS][index].ctr_loads = sumloads;
		per_cpu_logs[BWTIER_NR_CPUS][index].ctr_stores = sumstores;
		per_cpu_logs[BWTIER_NR_CPUS][index].nr_dram_load_samples = ndram_ld;
		per_cpu_logs[BWTIER_NR_CPUS][index].nr_cxl_load_samples = ncxl_ld;
		per_cpu_logs[BWTIER_NR_CPUS][index].nr_dram_store_samples = ndram_st;
		per_cpu_logs[BWTIER_NR_CPUS][index].nr_cxl_store_samples = ncxl_st;

		index = (index + 1) % SYS_RECORDS_HIST_LEN;
		memset(&per_cpu_logs[BWTIER_NR_CPUS][index], 0, sizeof(struct sys_stat_record));
		atomic_set(&current_record_index[BWTIER_NR_CPUS], index);
		per_cpu_logs[BWTIER_NR_CPUS][index].time_start = now;

		last = now;
		now = ktime_get_ns();
		tdelta_ms = (now - last) / NSEC_PER_MSEC;
		bwtier_msleep(stats_aggr_ms - tdelta_ms);
		ms_since_migr += stats_aggr_ms;
	}

	return 0;
}

static int kmigrtd_start(void)
{
	int cpu = -1;

	if (kmigrtd_task != NULL) {
		return -EBUSY;
	}

	per_cpu_logs = kzalloc(sizeof(struct sys_stat_record*) * (BWTIER_NR_CPUS + 1),
			GFP_KERNEL | __GFP_NOWARN);
	while ((cpu = cpumask_next(cpu, &cpu_bitmap)) < BWTIER_NR_CPUS) {
		per_cpu_logs[cpu] = vzalloc(sizeof(struct sys_stat_record) *
				SYS_RECORDS_HIST_LEN);
		atomic_set(&current_record_index[cpu], 0);
	}

	// totals
	per_cpu_logs[BWTIER_NR_CPUS] = vzalloc(sizeof(struct sys_stat_record) *
			SYS_RECORDS_HIST_LEN);
	atomic_set(&current_record_index[BWTIER_NR_CPUS], 0);

	kmigrtd_task = kthread_run(kmigrtd, NULL, "kmigrtd");
	return 0;
}

static int kmigrtd_stop(void)
{
	int cpu = -1;

	if (!kmigrtd_task) {
		return 0;
	}

	kthread_stop(kmigrtd_task);
	kmigrtd_task = NULL;

	while ((cpu = cpumask_next(cpu, &cpu_bitmap)) < BWTIER_NR_CPUS) {
		vfree(per_cpu_logs[cpu]);
	}
	vfree(per_cpu_logs[BWTIER_NR_CPUS]);
	kfree(per_cpu_logs);

	return 0;
}

void kmigrtd_init(void)
{
	atomic_set(&stats_ms, 100);
	atomic_set(&migr_ms, COOLING_PERIOD_MS);
	atomic_set(&max_migr_pages_per_round, 0);
}

int kmigrtd_enable(void)
{
	if (kmigrtd_task == NULL) {
		kmigrtd_start();
	}

	return 0;
}

int kmigrtd_disable(void)
{
	if (kmigrtd_task != NULL) {
		kmigrtd_stop();
	}

	return 0;
}

int bwtier_stats_ms(void)
{
	return atomic_read(&stats_ms);
}

int set_bwtier_stats_ms(int ms)
{
	atomic_set(&stats_ms, ms);
	return 0;
}

int bwtier_migr_ms(void)
{
	return atomic_read(&migr_ms);
}

int set_bwtier_migr_ms(int ms)
{
	atomic_set(&migr_ms, ms);
	return 0;
}

int set_max_migr_pages(int nrpages)
{
	atomic_set(&max_migr_pages_per_round, nrpages);
	return 0;
}

int max_migr_pages(void)
{
	return atomic_read(&max_migr_pages_per_round);
}