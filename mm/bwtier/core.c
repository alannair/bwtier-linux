// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER Core API
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmail.com>
 */

#include <linux/bwtier.h>

struct access_hist_bin access_histogram_bins[NUM_BWTIER_BINS];
static atomic_t oldest_bin_index; /* increment on COOLING */
static struct hrtimer htimer;
static ktime_t kt_periode;
static atomic_t cool_ms;

static int bin_index_from_access_count(int access_count)
{
	int x = ilog2(access_count);
	int index = atomic_read(&oldest_bin_index);

	if (x >= NUM_BWTIER_BINS)
		x = NUM_BWTIER_BINS - 1;

	return (x + index) % NUM_BWTIER_BINS;
}

static int bin_index_from_bin_id(int bin_id)
{
	int oldest_bin_index_val = atomic_read(&oldest_bin_index);
	int oldest_bin_id = access_histogram_bins[oldest_bin_index_val].bin_id;
	int diff = bin_id - oldest_bin_id;
	int index = (diff + oldest_bin_index_val) % NUM_BWTIER_BINS;

	if (diff < 0)
		return -ERR_BWTIER_INVAL_BININDEX;

	if (diff > NUM_BWTIER_BINS)
		return -ERR_INCOMPREHENSIBLE;

	if (access_histogram_bins[index].bin_id != bin_id)
		return -ERR_INCOMPREHENSIBLE;

	return index;
}

static void cool_once(void)
{
	int oldest_bin_index_val = atomic_read(&oldest_bin_index);
	int oldest_bin_id = access_histogram_bins[oldest_bin_index_val].bin_id;
	int new_bin_id = oldest_bin_id + NUM_BWTIER_BINS;
	struct list_head *headnext, *headnextnext;

	list_for_each_safe(headnext, headnextnext,
			&(access_histogram_bins[oldest_bin_index_val].pages_head)) {
		list_del(headnext);
	}
  
	access_histogram_bins[oldest_bin_index_val].nr_pages = 0;
	access_histogram_bins[oldest_bin_index_val].bin_id = new_bin_id;
	oldest_bin_index_val = (oldest_bin_index_val + 1) % NUM_BWTIER_BINS;
	atomic_set(&oldest_bin_index, oldest_bin_index_val);

	printk(KERN_INFO "Cooled Once %d\n", oldest_bin_index_val);
}

static enum hrtimer_restart do_lazy_cooling(struct hrtimer *timer)
{
	cool_once();
	hrtimer_forward_now(timer, kt_periode);
	return HRTIMER_RESTART;
}

static void lazy_cooling_enable(void)
{
	hrtimer_start(&htimer, kt_periode, HRTIMER_MODE_REL);
}

static void lazy_cooling_disable(void)
{
  hrtimer_cancel(&htimer);
}

void bwtier_core_init(void)
{
	int i;
	int cool_secs, cool_nsecs;

	atomic_set(&cool_ms, COOLING_PERIOD_MS);
	atomic_set(&oldest_bin_index, 0);

	cool_secs = COOLING_PERIOD_MS / 1000;
	cool_nsecs = (COOLING_PERIOD_MS % 1000) * 1000000;

	for (i = 0; i < NUM_BWTIER_BINS; i++) {
		access_histogram_bins[i].nr_pages = 0;
		access_histogram_bins[i].bin_id = i+1;
		INIT_LIST_HEAD(&(access_histogram_bins[i].pages_head));
	}

	kt_periode = ktime_set(cool_secs, cool_secs);
	hrtimer_init(&htimer, CLOCK_REALTIME, HRTIMER_MODE_REL);
	htimer.function = do_lazy_cooling;
}

static struct pginfo* update_base_page(struct vm_area_struct *vma, 
		struct page *page, uint64_t timestamp) 
{
	int bin_id, pg_bin_id, bin_index, pg_bin_index;
	uint64_t pg_acc_count, pg_last_cooled_ts;
	uint64_t cool_period_ns = (uint64_t)atomic_read(&cool_ms) * 1000000;
	struct pginfo *pginfo = kzalloc(sizeof(struct pginfo), GFP_KERNEL); 

	/* LOCK PAGE */
	pg_acc_count = page->access_count;
	pg_last_cooled_ts = page->last_cooled_timestamp;
	pg_bin_id = page->bin_id;
	pg_bin_index = bin_index_from_bin_id(pg_bin_id);
	/* UNLOCK PAGE */

	if (pg_bin_index < -ERR_UNRECOVERABLE) {
		printk(KERN_ERR "Unrecoverable Error\n");
		return NULL;
	}

	if (!pg_bin_id  || !pg_last_cooled_ts || pg_bin_index == -ERR_BWTIER_INVAL_BININDEX ||
			((timestamp - pg_last_cooled_ts) > (cool_period_ns * NUM_BWTIER_BINS))) {
		/* Page Sampled for the first time */
		page->access_count = 0;
		page->last_cooled_timestamp = timestamp;
	} else {
		while ((timestamp - pg_last_cooled_ts) > cool_period_ns) {
			page->access_count /= 2;
			pg_last_cooled_ts += cool_period_ns;
		}
		page->last_cooled_timestamp = pg_last_cooled_ts;
	}

	page->access_count += 512;
	bin_index = bin_index_from_access_count(page->access_count);
	bin_id = access_histogram_bins[bin_index].bin_id;

	if (pg_bin_id != bin_id) {
		if (pg_bin_index != -ERR_BWTIER_INVAL_BININDEX) {
			access_histogram_bins[pg_bin_index].nr_pages--;
			list_move(&(page->bwtier_list),
				  &(access_histogram_bins[bin_index].pages_head));
		} else {
			list_add(&(page->bwtier_list),
				  &(access_histogram_bins[bin_index].pages_head));
		}

		access_histogram_bins[bin_index].nr_pages++;
		page->bin_id = bin_id;
	}

	pginfo->bin_id = bin_id;
	pginfo->nid = page_to_nid(page);

	return pginfo;
}

static struct pginfo* update_huge_page(struct vm_area_struct *vma,
		pmd_t *pmd, struct page *page, uint64_t address, uint64_t timestamp)
{
	return NULL;
}

static struct pginfo* __update_pte_pginfo(struct vm_area_struct *vma, 
		pmd_t *pmd, uint64_t address, uint64_t timestamp)
{
	pte_t *pte, ptent;
	spinlock_t *ptl;
	struct page *page;
	struct pginfo *pginfo = NULL;

	pte = pte_offset_map_lock(vma->vm_mm, pmd, address, &ptl);
	ptent = *pte;
	if (!pte_present(ptent))
		goto pte_unlock;

	page = vm_normal_page(vma, address, ptent);
	if (!page || PageKsm(page))
		goto pte_unlock;

	if (page != compound_head(page))
		goto pte_unlock;

	pginfo = update_base_page(vma, page, timestamp);

pte_unlock:
	pte_unmap_unlock(pte, ptl);
	return pginfo;
}

static struct pginfo* __update_pmd_pginfo(struct vm_area_struct *vma, 
		pud_t *pud, uint64_t address, uint64_t timestamp)
{
	pmd_t *pmd, pmdval;
	struct page *page;

	pmd = pmd_offset(pud, address);
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

		return update_huge_page(vma, pmd, page, address, timestamp);
	} else {
		/* base page */
		return __update_pte_pginfo(vma, pmd, address, timestamp);
	}

out:
	return NULL;
}

static struct pginfo* __update_pginfo(struct vm_area_struct *vma, 
		uint64_t addr, uint64_t timestamp)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	pgd = pgd_offset(vma->vm_mm, addr);
	if (pgd_none_or_clear_bad(pgd))
		return NULL;

	p4d = p4d_offset(pgd, addr);
	if (p4d_none_or_clear_bad(p4d))
		return NULL;

	pud = pud_offset(p4d, addr);
	if (pud_none_or_clear_bad(pud))
		return NULL;

	return __update_pmd_pginfo(vma, pud, addr, timestamp);
}

struct pginfo* update_pginfo(struct bwtier_sample *sample)
{
	pid_t pid = sample->pid;
	uint64_t address = sample->addr, timestamp = sample->time;
	struct pid *pid_struct = find_get_pid(pid);
	struct task_struct *p = pid_struct ? 
			pid_task(pid_struct, PIDTYPE_PID) : NULL;
	struct mm_struct *mm = p ? p->mm : NULL;
	struct vm_area_struct *vma;
	struct pginfo *pginfo = NULL;

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

	pginfo = __update_pginfo(vma, address, timestamp);

mmap_unlock:
	mmap_read_unlock(mm);
put_task:
	if (pid_struct)
		put_pid(pid_struct);
	return pginfo;
}

int bwtier_cool_ms(void)
{
	return atomic_read(&cool_ms);
}

int set_bwtier_cool_ms(int ms)
{
	int secs, nsecs;

	atomic_set(&cool_ms, ms);
	secs = ms / 1000;
	nsecs = (ms % 1000) * 1000000;
  kt_periode = ktime_set(secs, nsecs);
	return 0;
}

int bwtier_enable(void)
{
	lazy_cooling_enable();
	ksampld_enable();
	return 0;
}

int bwtier_disable(void)
{
	lazy_cooling_disable();
	ksampld_disable();
	return 0;
}
