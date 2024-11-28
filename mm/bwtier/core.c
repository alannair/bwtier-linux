// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER Core API
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmail.com>
 */

#include <linux/bwtier.h>

#include "../internal.h"

atomic_t bwtier_status_var;

struct access_hist_bin* pg_hist_bins;
atomic_t oldest_bin_index;
atomic_t newest_bin_index;

static int is_some_list_entry(struct list_head *entry)
{
	/* entry assumed to point to a legit list_head */
	if (entry->next == LIST_POISON1 || entry->prev == LIST_POISON2 ||
			entry->next == NULL || entry->prev == NULL)
		return 0;
	return 1;
}

static void update_sample_counts(int cpu, bool is_load, bool is_cxl)
{
	int curr_record_index = atomic_read(&current_record_index[cpu]);

	if (is_load) {
		if (!is_cxl)
			per_cpu_logs[cpu][curr_record_index].nr_dram_load_samples++;
		else
			per_cpu_logs[cpu][curr_record_index].nr_cxl_load_samples++;
	} else {
		if (!is_cxl)
			per_cpu_logs[cpu][curr_record_index].nr_dram_store_samples++;
		else
			per_cpu_logs[cpu][curr_record_index].nr_cxl_store_samples++;
	}
}

struct folio *bwtier_get_folio(struct page *page)
{
	struct folio *folio;

	if (!page || PageTail(page))
		return NULL;

	folio = page_folio(page);
	if (!folio)
		return NULL;

	if (!folio_try_get(folio))
		return NULL;

	return folio;
}

void bwtier_msleep(unsigned long msecs)
{
	unsigned long usecs = msecs * 1000;

	if (msecs > 20)
		schedule_timeout_idle(msecs_to_jiffies(msecs));
	else
		usleep_idle_range(usecs, usecs + 1);
}

int bin_index_from_access_count(int access_count)
{
	int oindex = atomic_read(&oldest_bin_index);
	int nindex = atomic_read(&newest_bin_index);
	int x = ilog2(access_count);
	int n = (nindex - oindex + NUM_BWTIER_BINS) % NUM_BWTIER_BINS;

	if (x >= n)
		x = n;

	return (x + oindex) % NUM_BWTIER_BINS;
}

int bin_index_from_bin_id(int bin_id)
{
	int index, oldest_bin_id, newest_bin_id;
	int oldest_bin_index_val, newest_bin_index_val;

	oldest_bin_index_val = atomic_read(&oldest_bin_index);
	newest_bin_index_val = atomic_read(&newest_bin_index);
	oldest_bin_id = pg_hist_bins[oldest_bin_index_val].bin_id;
	newest_bin_id = pg_hist_bins[newest_bin_index_val].bin_id;

	if ((bin_id < oldest_bin_id))
		return -ERR_BWTIER_INVAL_BININDEX;

	if (bin_id > newest_bin_id) {
		printk(KERN_ERR "Unrecoverable Error (?) %d %d %d\n",
				bin_id, newest_bin_id, oldest_bin_id);
		return -ERR_INCOMPREHENSIBLE;
	}

	index = (oldest_bin_index_val + (bin_id - oldest_bin_id)) % NUM_BWTIER_BINS;

	if (pg_hist_bins[index].bin_id != bin_id) {
		printk(KERN_ERR "Unrecoverable Error (??) %d %d %d\n",
				index, pg_hist_bins[index].bin_id, bin_id);
		return -ERR_INCOMPREHENSIBLE;
	}

	return index;
}

void cool_once(void)
{
	int tmp, nextnewindex, oldest_index_val, newest_index_val, new_bin_id;
	struct folio *folio;

	oldest_index_val = atomic_read(&oldest_bin_index);
	newest_index_val = atomic_read(&newest_bin_index);
	new_bin_id = pg_hist_bins[newest_index_val].bin_id + 1;
	nextnewindex = (newest_index_val + 1) % NUM_BWTIER_BINS;

	mutex_lock(&(pg_hist_bins[nextnewindex].lock));

	if (nextnewindex == oldest_index_val) {
		tmp = (oldest_index_val + 1) % NUM_BWTIER_BINS;
		atomic_set(&oldest_bin_index, tmp);

		while (!list_empty(&(pg_hist_bins[oldest_index_val].dram_pages_head))) {
			folio = lru_to_folio(&(pg_hist_bins[oldest_index_val].dram_pages_head));
			list_del(&(folio->lru));
			if (!folio_try_get(folio))
				continue;
			folio_putback_lru(folio);
		}

		while (!list_empty(&(pg_hist_bins[oldest_index_val].cxl_pages_head))) {
			folio = lru_to_folio(&(pg_hist_bins[oldest_index_val].cxl_pages_head));
			list_del(&(folio->lru));
			if (!folio_try_get(folio))
				continue;
			folio_putback_lru(folio);
		}
	}

	pg_hist_bins[nextnewindex].nr_dram_pages = 0;
	pg_hist_bins[nextnewindex].nr_cxl_pages = 0;
	pg_hist_bins[nextnewindex].bin_id = new_bin_id;
	INIT_LIST_HEAD(&(pg_hist_bins[nextnewindex].dram_pages_head));
	INIT_LIST_HEAD(&(pg_hist_bins[nextnewindex].cxl_pages_head));
	atomic_set(&newest_bin_index, nextnewindex);

	mutex_unlock(&(pg_hist_bins[nextnewindex].lock));

	printk(KERN_INFO "Cooled Once %d %d\n", nextnewindex, new_bin_id);
}

void bwtier_core_init(void)
{
	int i;

	atomic_set(&bwtier_status_var, 0);
	atomic_set(&oldest_bin_index, 0);
	atomic_set(&newest_bin_index, 1);
	pg_hist_bins = vzalloc(sizeof(struct access_hist_bin) *
			NUM_BWTIER_BINS);

	for (i = 0; i < NUM_BWTIER_BINS; i++) {
		pg_hist_bins[i].nr_dram_pages = 0;
		pg_hist_bins[i].nr_cxl_pages = 0;
		pg_hist_bins[i].bin_id = i+1;
		INIT_LIST_HEAD(&(pg_hist_bins[i].dram_pages_head));
		INIT_LIST_HEAD(&(pg_hist_bins[i].cxl_pages_head));
		mutex_init(&(pg_hist_bins[i].lock));
	}
}

static int update_base_page(struct vm_area_struct *vma, 
		struct page *page, struct bwtier_sample *sample, bool is_load)
{
	uint64_t timestamp, pg_last_cooled_ts, migr_ns;
	int pg_bin_id, pg_acc_count, nid, iscxl, pg_bin_index;
	int bin_id, bin_index, cpu, ret = 0;
	struct folio *folio = bwtier_get_folio(page);

	migr_ns = (uint64_t)atomic_read(&migr_ms) * NSEC_PER_MSEC;
	timestamp = sample->timestamp;
	cpu = sample->cpu;

	if (!folio) {
		ret = -ERR_BWTIER_NO_FOLIO;
		goto put_folio;
	}

	if (!folio_trylock(folio)) {
		ret = -ERR_BWTIER_FOLIO_LOCK;
		goto put_folio;
	}

	nid = folio_nid(folio);
	pg_acc_count = atomic_read(&page->access_count);
	pg_bin_id = atomic_read(&page->bin_id);
	pg_bin_index = bin_index_from_bin_id(pg_bin_id);
	pg_last_cooled_ts = atomic64_read(&page->last_cooled_timestamp);

	if (nid == 2 || nid == 3) {
		iscxl = 1;
	} else if (nid == 0 || nid == 1) {
		iscxl = 0;
	} else {
		ret = -ERR_BWTIER_INVAL_NID;
		goto unlock;
	}

	if (!pg_bin_id || !pg_last_cooled_ts || 
			pg_bin_index == -ERR_BWTIER_INVAL_BININDEX ||
			((timestamp - pg_last_cooled_ts) > (migr_ns * NUM_BWTIER_BINS))) {
		/* Page Sampled for the first time */
		atomic_set(&page->access_count, 0);
		atomic64_set(&page->last_cooled_timestamp, timestamp);
	} else {
		while ((timestamp - pg_last_cooled_ts) > migr_ns) {
			pg_acc_count /= 2;
			pg_last_cooled_ts += migr_ns;
		}
		atomic_set(&page->access_count, pg_acc_count);
		atomic64_set(&page->last_cooled_timestamp, pg_last_cooled_ts);
	}

	/* add scaled(sample->period) instead of 512 */
	pg_acc_count = atomic_add_return(512, &page->access_count);
	bin_index = bin_index_from_access_count(pg_acc_count);
	bin_id = pg_hist_bins[bin_index].bin_id;

	// printk(KERN_INFO "Bin Index %d %d %d %d %d %d %d %d\n",
	// 		bin_index, bin_id, pg_acc_count, pg_bin_id, pg_bin_index,
	// 		pg_hist_bins[bin_index].nr_dram_pages,
	// 		pg_hist_bins[bin_index].nr_cxl_pages, iscxl);

	if (bin_id != pg_bin_id) {
		if ((!is_some_list_entry(&(folio->lru))) || folio_isolate_lru(folio)) {
			/* either folio was never part of any list (cold page)
			 * or folio was on LRU list, now removed by us
			 */
			if (pg_bin_index != -ERR_BWTIER_INVAL_BININDEX) {
				/* ERROR */
				ret = -ERR_INCOMPREHENSIBLE;
				goto unlock;
			}

			// printk(KERN_INFO "Adding\n");

			ret = 1;
			mutex_lock(&(pg_hist_bins[bin_index].lock));
			if (!iscxl) {
				list_add(&(folio->lru), &(pg_hist_bins[bin_index].dram_pages_head));
				pg_hist_bins[bin_index].nr_dram_pages++;
			} else {
				list_add(&(folio->lru), &(pg_hist_bins[bin_index].cxl_pages_head));
				pg_hist_bins[bin_index].nr_cxl_pages++;
			}
			mutex_unlock(&(pg_hist_bins[bin_index].lock));
		} else {
			/* folio must be in one of our lists, exp. at pg_bin_index */
			if (pg_bin_index == -ERR_BWTIER_INVAL_BININDEX) {
				/* ERROR */
				ret = -ERR_INCOMPREHENSIBLE;
				goto unlock;
			}

			// printk(KERN_INFO "Moving\n");

			ret = 2;
			mutex_lock(&(pg_hist_bins[pg_bin_index].lock));
			mutex_lock(&(pg_hist_bins[bin_index].lock));
			if (!iscxl) {
				list_move(&(folio->lru), &(pg_hist_bins[bin_index].dram_pages_head));
				pg_hist_bins[pg_bin_index].nr_dram_pages--;
				pg_hist_bins[bin_index].nr_dram_pages++;
			} else {
				list_move(&(folio->lru), &(pg_hist_bins[bin_index].cxl_pages_head));
				pg_hist_bins[pg_bin_index].nr_cxl_pages--;
				pg_hist_bins[bin_index].nr_cxl_pages++;
			}
			mutex_unlock(&(pg_hist_bins[bin_index].lock));
			mutex_unlock(&(pg_hist_bins[pg_bin_index].lock));
		}
	}

	update_sample_counts(cpu, is_load, iscxl);

unlock:
	folio_unlock(folio);
put_folio:
	folio_put(folio);
	return ret;
}

// static int update_base_page(struct vm_area_struct *vma, 
// 		struct page *page, struct bwtier_sample *sample, bool is_load) 
// {
// 	uint64_t timestamp = sample->timestamp;
// 	int bin_id, pg_bin_id, bin_index, pg_bin_index, curr_record_index;
// 	uint64_t pg_acc_count, pg_last_cooled_ts;
// 	uint64_t migr_ns = (uint64_t)atomic_read(&migr_ms) * 1000000;
// 	int nid = page_to_nid(page), iscxl = 0, cpu = sample->cpu;

// 	if (nid == 2 || nid == 3)
// 		iscxl = 1;

// 	/* LOCK PAGE */
// 	pg_acc_count = page->access_count;
// 	pg_last_cooled_ts = page->last_cooled_timestamp;
// 	pg_bin_id = page->bin_id;
// 	pg_bin_index = bin_index_from_bin_id(pg_bin_id);
// 	/* UNLOCK PAGE */

// 	if (!pg_bin_id  || !pg_last_cooled_ts || 
// 			pg_bin_index == -ERR_BWTIER_INVAL_BININDEX ||
// 			((timestamp - pg_last_cooled_ts) > (migr_ns * NUM_BWTIER_BINS))) {
// 		/* Page Sampled for the first time */
// 		page->access_count = 0;
// 		page->last_cooled_timestamp = timestamp;
// 	} else {
// 		while ((timestamp - pg_last_cooled_ts) > migr_ns) {
// 			page->access_count /= 2;
// 			pg_last_cooled_ts += migr_ns;
// 		}
// 		page->last_cooled_timestamp = pg_last_cooled_ts;
// 	}

// 	page->access_count += 512; // something related to period
// 	bin_index = bin_index_from_access_count(page->access_count);
// 	bin_id = pg_hist_bins[bin_index].bin_id;

// 	if (pg_bin_id != bin_id) {
// 		if (pg_bin_index != -ERR_BWTIER_INVAL_BININDEX) {
// 			if (iscxl)
// 				pg_hist_bins[pg_bin_index].nr_cxl_pages--;
// 			else
// 				pg_hist_bins[pg_bin_index].nr_dram_pages--;

// 			if (!is_some_list_entry(&(page->bwtier_list))) {
// 				/* There is some mistake in our logic. Skip this sample for now. */
// 				printk(KERN_ERR "list_move: Not Part of any list %d %d %d %d %llx %llx\n",
// 						page->bin_id, page->access_count, bin_id, bin_index,
// 						(unsigned long long)page->bwtier_list.next,
// 						(unsigned long long)page->bwtier_list.prev);
// 			} else {
// 				mutex_lock(&(pg_hist_bins[pg_bin_index].lock));
// 				mutex_lock(&(pg_hist_bins[bin_index].lock));
// 				if (iscxl) {
// 					list_move(&(page->bwtier_list),
// 						  &(pg_hist_bins[bin_index].cxl_pages_head));
// 				} else {
// 					list_move(&(page->bwtier_list),
// 						  &(pg_hist_bins[bin_index].dram_pages_head));
// 				}
// 				mutex_unlock(&(pg_hist_bins[bin_index].lock));
// 				mutex_unlock(&(pg_hist_bins[pg_bin_index].lock));
// 			}
// 		} else {
// 			mutex_lock(&(pg_hist_bins[bin_index].lock));
// 			if (iscxl) {
// 				list_add(&(page->bwtier_list),
// 						&(pg_hist_bins[bin_index].cxl_pages_head));
// 			} else {
// 				list_add(&(page->bwtier_list),
// 						&(pg_hist_bins[bin_index].dram_pages_head));
// 			}
// 			mutex_unlock(&(pg_hist_bins[bin_index].lock));
// 		}

// 		if (iscxl)
// 			pg_hist_bins[bin_index].nr_cxl_pages++;
// 		else
// 			pg_hist_bins[bin_index].nr_dram_pages++;
// 		page->bin_id = bin_id;
// 	}

// 	/* Update System Stats Records */
// 	curr_record_index = atomic_read(&current_record_index[cpu]);

// 	if (is_load) {
// 		if (!iscxl)
// 			per_cpu_logs[cpu][curr_record_index].nr_dram_load_samples++;
// 		else
// 			per_cpu_logs[cpu][curr_record_index].nr_cxl_load_samples++;
// 	} else {
// 		if (!iscxl)
// 			per_cpu_logs[cpu][curr_record_index].nr_dram_store_samples++;
// 		else
// 			per_cpu_logs[cpu][curr_record_index].nr_cxl_store_samples++;
// 	}

// 	return 0;
// }

static int update_huge_page(struct vm_area_struct *vma, pmd_t *pmd,
		struct page *page, struct bwtier_sample *sample, bool is_load)
{
	return 0;
}

static int __update_pte_pginfo(struct vm_area_struct *vma, 
		pmd_t *pmd, struct bwtier_sample *sample, bool is_load)
{
	pte_t *pte, ptent;
	spinlock_t *ptl;
	struct page *page;
	int ret;

	pte = pte_offset_map_lock(vma->vm_mm, pmd, sample->address, &ptl);
	ptent = *pte;
	if (!pte_present(ptent))
		goto pte_unlock;

	page = vm_normal_page(vma, sample->address, ptent);
	if (!page || PageKsm(page))
		goto pte_unlock;

	if (page != compound_head(page))
		goto pte_unlock;

	ret = update_base_page(vma, page, sample, is_load);

pte_unlock:
	pte_unmap_unlock(pte, ptl);
	return ret;
}

static int __update_pmd_pginfo(struct vm_area_struct *vma, 
		pud_t *pud, struct bwtier_sample *sample, bool is_load)
{
	pmd_t *pmd, pmdval;
	struct page *page;

	pmd = pmd_offset(pud, sample->address);
	if (!pmd || pmd_none(*pmd))
		goto out;
    
	if (is_swap_pmd(*pmd))
		goto out;

	if (!pmd_trans_huge(*pmd) && !pmd_devmap(*pmd) && 
			unlikely(pmd_bad(*pmd))) {
		pmd_clear_bad(pmd);
		goto out;
	}

	pmdval = *pmd;
	if (pmd_trans_huge(pmdval) || pmd_devmap(pmdval)) {
		if (is_huge_zero_pmd(pmdval))
			goto out;
	
		page = pmd_page(pmdval);
		if (!page)
			goto out;

		if (!PageCompound(page))
			goto out;

		return update_huge_page(vma, pmd, page, sample, is_load);
	} else {
		/* base page */
		return __update_pte_pginfo(vma, pmd, sample, is_load);
	}

out:
	return -1;
}

static int __update_pginfo(struct vm_area_struct *vma, 
		struct bwtier_sample *sample, bool is_load)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	pgd = pgd_offset(vma->vm_mm, sample->address);
	if (pgd_none_or_clear_bad(pgd))
		return -1;

	p4d = p4d_offset(pgd, sample->address);
	if (p4d_none_or_clear_bad(p4d))
		return -1;

	pud = pud_offset(p4d, sample->address);
	if (pud_none_or_clear_bad(pud))
		return -1;

	return __update_pmd_pginfo(vma, pud, sample, is_load);
}

int update_pginfo(struct bwtier_sample *sample, bool is_load)
{
	pid_t pid = sample->pid;
	uint64_t address = sample->address;
	struct pid *pid_struct = find_get_pid(pid);
	struct task_struct *p = pid_struct ? 
			pid_task(pid_struct, PIDTYPE_PID) : NULL;
	struct mm_struct *mm = p ? p->mm : NULL;
	struct vm_area_struct *vma;

	if (!mm)
		goto put_task;

	if (!mmap_read_trylock(mm))
		goto put_task;

	vma = find_vma(mm, address);
	if (unlikely(!vma))
		goto mmap_unlock;

	if (!vma->vm_mm || !vma_migratable(vma))
		goto mmap_unlock;

	if (vma->vm_file) {
		if ((vma->vm_flags & (VM_READ | VM_WRITE)) == VM_READ)
			goto mmap_unlock;
	}

	__update_pginfo(vma, sample, is_load);

mmap_unlock:
	mmap_read_unlock(mm);
put_task:
	if (pid_struct)
		put_pid(pid_struct);
	return 0;
}

int bwtier_statistics(char *kbuf, int buflen)
{
	int index, nextindex, curr_index, old_index, len = 0;
	uint64_t start, dur, end;
	uint64_t loads, stores,dr_loads, dr_stores, cx_loads, cx_stores;

	if (atomic_read(&bwtier_status_var) == 0)
		return 0;

	if (!kbuf)
		return -EINVAL;

	old_index =	atomic_read(&oldest_bin_index);
	index = atomic_read(&newest_bin_index);
	while (index != old_index) {
		len += scnprintf(kbuf + len, buflen - len, "[%d : (%d/%d)]\t",
				pg_hist_bins[index].bin_id, pg_hist_bins[index].nr_dram_pages,
				pg_hist_bins[index].nr_cxl_pages);
		index = (index + NUM_BWTIER_BINS - 1) % NUM_BWTIER_BINS;
	}
	len += scnprintf(kbuf + len, buflen - len, "[%d : (%d/%d)]\n",
			pg_hist_bins[old_index].bin_id, pg_hist_bins[old_index].nr_dram_pages,
			pg_hist_bins[old_index].nr_cxl_pages);

	curr_index = atomic_read(&current_record_index[BWTIER_NR_CPUS]);
	index = (curr_index + SYS_RECORDS_HIST_LEN - 1) % SYS_RECORDS_HIST_LEN;
	nextindex = (index + SYS_RECORDS_HIST_LEN - 1) % SYS_RECORDS_HIST_LEN;
	while (nextindex != curr_index) {
		start = per_cpu_logs[BWTIER_NR_CPUS][index].time_start;
		dur = per_cpu_logs[BWTIER_NR_CPUS][index].time_dur_ns;
		end = start + dur;
		len += scnprintf(kbuf + len, buflen - len, "Rec %d (%llu-%llu:%llu)\t", 
				index, start, end, dur);

		loads = per_cpu_logs[BWTIER_NR_CPUS][index].ctr_loads - 
				per_cpu_logs[BWTIER_NR_CPUS][nextindex].ctr_loads;
		stores = per_cpu_logs[BWTIER_NR_CPUS][index].ctr_stores -
				per_cpu_logs[BWTIER_NR_CPUS][nextindex].ctr_stores;
		len += scnprintf(kbuf + len, buflen - len, "Loads: %llu MB/s\t", 
				((loads * NSEC_PER_SEC * 64) / (dur * 1ULL << 20)));
		len += scnprintf(kbuf + len, buflen - len, "Stores: %llu MB/s\t", 
				((stores  * NSEC_PER_SEC * 64) / (dur * 1ULL << 20)));

		dr_loads = per_cpu_logs[BWTIER_NR_CPUS][index].nr_dram_load_samples;
		dr_stores = per_cpu_logs[BWTIER_NR_CPUS][index].nr_dram_store_samples;
		cx_loads = per_cpu_logs[BWTIER_NR_CPUS][index].nr_cxl_load_samples;
		cx_stores = per_cpu_logs[BWTIER_NR_CPUS][index].nr_cxl_store_samples;
		len += scnprintf(kbuf + len, buflen - len, "DR.Ld: %llu\t", dr_loads);
		len += scnprintf(kbuf + len, buflen - len, "DR.St: %llu\t", dr_stores);
		len += scnprintf(kbuf + len, buflen - len, "CX.Ld: %llu\t", cx_loads);
		len += scnprintf(kbuf + len, buflen - len, "CX.St: %llu\n",	cx_stores);

		if (len + 512 >= buflen) {
			break;
		}
		index = nextindex;
		nextindex = (index + SYS_RECORDS_HIST_LEN - 1) % SYS_RECORDS_HIST_LEN;
	}
	kbuf[len] = '\0';

	return len + 1;
}

int bwtier_enable(void)
{
	atomic_set(&bwtier_status_var, 1);
	ksampld_enable();
	kmigrtd_enable();
	return 0;
}

int bwtier_disable(void)
{
	atomic_set(&bwtier_status_var, 0);
	ksampld_disable();
	kmigrtd_disable();
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

bool bwtier_status(void)
{
	return atomic_read(&bwtier_status_var);
}
