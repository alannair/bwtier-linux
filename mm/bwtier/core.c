// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER Core API
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmail.com>
 */

#include <linux/bwtier.h>

atomic_t bwtier_status_var;

struct access_hist_bin* pg_hist_bins;
atomic_t oldest_bin_index;

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
	int x = ilog2(access_count);
	int index = atomic_read(&oldest_bin_index);

	if (x >= NUM_BWTIER_BINS)
		x = NUM_BWTIER_BINS - 1;

	return (x + index) % NUM_BWTIER_BINS;
}

int bin_index_from_bin_id(int bin_id)
{
	int oldest_bin_index_val = atomic_read(&oldest_bin_index);
	int oldest_bin_id = pg_hist_bins[oldest_bin_index_val].bin_id;
	int diff = bin_id - oldest_bin_id;
	int index = (diff + oldest_bin_index_val) % NUM_BWTIER_BINS;

	if (diff < 0)
		return -ERR_BWTIER_INVAL_BININDEX;

	if (diff > NUM_BWTIER_BINS) {
		printk(KERN_ERR "Unrecoverable Error diff=%d (%d-%d)\n",
				diff, bin_id, oldest_bin_id);
		return -ERR_INCOMPREHENSIBLE;
	}

	if (pg_hist_bins[index].bin_id != bin_id) {
		printk(KERN_ERR "Unrecoverable Error %d %d %d\n",
				index, pg_hist_bins[index].bin_id, bin_id);
		return -ERR_INCOMPREHENSIBLE;
	}

	return index;
}

void cool_once(void)
{
	int tmp, oldest_bin_index_val = atomic_read(&oldest_bin_index);
	int oldest_bin_id = pg_hist_bins[oldest_bin_index_val].bin_id;
	int new_bin_id = oldest_bin_id + NUM_BWTIER_BINS;
	struct list_head *headnext, *headnextnext;

	tmp = (oldest_bin_index_val + 1) % NUM_BWTIER_BINS;
	atomic_set(&oldest_bin_index, tmp);

	mutex_lock(&(pg_hist_bins[oldest_bin_index_val].lock));
	list_for_each_safe(headnext, headnextnext,
			&(pg_hist_bins[oldest_bin_index_val].dram_pages_head)) {
		list_del(headnext);
	}
	list_for_each_safe(headnext, headnextnext,
			&(pg_hist_bins[oldest_bin_index_val].cxl_pages_head)) {
		list_del(headnext);
	}
	mutex_unlock(&(pg_hist_bins[oldest_bin_index_val].lock));

	pg_hist_bins[oldest_bin_index_val].nr_dram_pages = 0;
	pg_hist_bins[oldest_bin_index_val].nr_cxl_pages = 0;
	pg_hist_bins[oldest_bin_index_val].bin_id = new_bin_id;
	printk(KERN_INFO "Cooled Once %d\n", oldest_bin_index_val);
}

void bwtier_core_init(void)
{
	int i;

	atomic_set(&bwtier_status_var, 0);
	atomic_set(&oldest_bin_index, 0);
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
	uint64_t timestamp = sample->timestamp;
	int bin_id, pg_bin_id, bin_index, pg_bin_index, curr_record_index;
	uint64_t pg_acc_count, pg_last_cooled_ts;
	uint64_t migr_ns = (uint64_t)atomic_read(&migr_ms) * 1000000;
	int nid = page_to_nid(page), iscxl = 0;

	if (nid == 2 || nid == 3)
		iscxl = 1;

	/* LOCK PAGE */
	pg_acc_count = page->access_count;
	pg_last_cooled_ts = page->last_cooled_timestamp;
	pg_bin_id = page->bin_id;
	pg_bin_index = bin_index_from_bin_id(pg_bin_id);
	/* UNLOCK PAGE */

	if (!pg_bin_id  || !pg_last_cooled_ts || 
			pg_bin_index == -ERR_BWTIER_INVAL_BININDEX ||
			((timestamp - pg_last_cooled_ts) > (migr_ns * NUM_BWTIER_BINS))) {
		/* Page Sampled for the first time */
		page->access_count = 0;
		page->last_cooled_timestamp = timestamp;
	} else {
		while ((timestamp - pg_last_cooled_ts) > migr_ns) {
			page->access_count /= 2;
			pg_last_cooled_ts += migr_ns;
		}
		page->last_cooled_timestamp = pg_last_cooled_ts;
	}

	page->access_count += 512; // something related to period
	bin_index = bin_index_from_access_count(page->access_count);
	bin_id = pg_hist_bins[bin_index].bin_id;

	if (pg_bin_id != bin_id) {
		if (pg_bin_index != -ERR_BWTIER_INVAL_BININDEX) {
			if (iscxl)
				pg_hist_bins[pg_bin_index].nr_cxl_pages--;
			else
				pg_hist_bins[pg_bin_index].nr_dram_pages--;

			mutex_lock(&(pg_hist_bins[pg_bin_index].lock));
			mutex_lock(&(pg_hist_bins[bin_index].lock));
			if (iscxl) {
				list_move(&(page->bwtier_list),
					  &(pg_hist_bins[bin_index].cxl_pages_head));
			} else {
				list_move(&(page->bwtier_list),
					  &(pg_hist_bins[bin_index].dram_pages_head));
			}
			mutex_unlock(&(pg_hist_bins[bin_index].lock));
			mutex_unlock(&(pg_hist_bins[pg_bin_index].lock));
		} else {
			mutex_lock(&(pg_hist_bins[bin_index].lock));
			if (iscxl) {
				list_add(&(page->bwtier_list),
						&(pg_hist_bins[bin_index].cxl_pages_head));
			} else {
				list_add(&(page->bwtier_list),
						&(pg_hist_bins[bin_index].dram_pages_head));
			}
			mutex_unlock(&(pg_hist_bins[bin_index].lock));
		}

		if (iscxl)
			pg_hist_bins[bin_index].nr_cxl_pages++;
		else
			pg_hist_bins[bin_index].nr_dram_pages++;
		page->bin_id = bin_id;
	}

	/* Update System Stats Records */
	curr_record_index = atomic_read(&current_record_index[sample->cpu]);

	if (is_load) {
		if (!iscxl)
			per_cpu_logs[sample->cpu][curr_record_index].nr_dram_load_samples++;
		else
			per_cpu_logs[sample->cpu][curr_record_index].nr_cxl_load_samples++;
	} else {
		if (!iscxl)
			per_cpu_logs[sample->cpu][curr_record_index].nr_dram_store_samples++;
		else
			per_cpu_logs[sample->cpu][curr_record_index].nr_cxl_store_samples++;
	}

	return 0;
}

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
	int index, curr_index, old_index, len = 0;

	if (atomic_read(&bwtier_status_var) == 0)
		return 0;

	if (!kbuf)
		return -EINVAL;

	old_index =	atomic_read(&oldest_bin_index);
	index = (old_index + NUM_BWTIER_BINS - 1) % NUM_BWTIER_BINS;
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
	while (index != curr_index) {
		len += scnprintf(kbuf + len, buflen - len, "Rec %d (%llu-%llu)\t",
				index, per_cpu_logs[BWTIER_NR_CPUS][index].time_start,
				per_cpu_logs[BWTIER_NR_CPUS][index].time_start +
				per_cpu_logs[BWTIER_NR_CPUS][index].time_dur_ns);
		len += scnprintf(kbuf + len, buflen - len, "DR.Ld: %llu\t",
				per_cpu_logs[BWTIER_NR_CPUS][index].nr_dram_load_samples);
		len += scnprintf(kbuf + len, buflen - len, "DR.St: %llu\t",
				per_cpu_logs[BWTIER_NR_CPUS][index].nr_dram_store_samples);
		len += scnprintf(kbuf + len, buflen - len, "CX.Ld: %llu\t",
				per_cpu_logs[BWTIER_NR_CPUS][index].nr_cxl_load_samples);
		len += scnprintf(kbuf + len, buflen - len, "CX.St: %llu\t",
				per_cpu_logs[BWTIER_NR_CPUS][index].nr_cxl_store_samples);
		len += scnprintf(kbuf + len, buflen - len, "Loads: %llu MB\t",
				(per_cpu_logs[BWTIER_NR_CPUS][index].ctr_loads * 64) / (1ULL << 20));
		len += scnprintf(kbuf + len, buflen - len, "Stores: %llu MB\n",
				(per_cpu_logs[BWTIER_NR_CPUS][index].ctr_stores * 64) / (1ULL << 20));

		if (len + 512 >= buflen) {
			break;
		}
		index--;
		if (index < 0)
			index = SYS_RECORDS_HIST_LEN - 1;
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
