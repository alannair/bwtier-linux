// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER Core API
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmail.com>
 */

#include <linux/bwtier.h>

struct access_hist_bin access_histogram_bins[NUM_BWTIER_BINS];
int oldest_bin_index = 0; /* increment on COOLING */
static struct hrtimer htimer;
static ktime_t kt_periode;

static int bin_index(int access_count)
{
	int bin_id = ilog2(access_count);

	if (bin_id >= NUM_BWTIER_BINS)
		bin_id = NUM_BWTIER_BINS - 1;

	return (bin_id + oldest_bin_index) % NUM_BWTIER_BINS;
}

static void cool_once(void)
{
	uint32_t new_bin_id = access_histogram_bins[oldest_bin_index].bin_id + 1;
	struct list_head *lh1, *lh2;

	list_for_each_safe(lh1, lh2,
			&(access_histogram_bins[oldest_bin_index].pages_head)) {
		list_del(lh1);
	}

	access_histogram_bins[oldest_bin_index].nr_pages = 0;
	access_histogram_bins[oldest_bin_index].bin_id = new_bin_id;
	oldest_bin_index = (oldest_bin_index + 1) % NUM_BWTIER_BINS;

	printk(KERN_INFO "Cooled Once %d\n", oldest_bin_index);
}

static enum hrtimer_restart do_cooling(struct hrtimer *timer)
{
	cool_once();
	hrtimer_forward_now(timer, kt_periode);
	return HRTIMER_RESTART;
}

static void periodic_cooling_enable(int secs, int nsecs)
{
	kt_periode = ktime_set(secs, nsecs);
	hrtimer_init(&htimer, CLOCK_REALTIME, HRTIMER_MODE_REL);
	htimer.function = do_cooling;
	hrtimer_start(&htimer, kt_periode, HRTIMER_MODE_REL);
}

static void periodic_cooling_disable(void)
{
  hrtimer_cancel(&htimer);
}

void bwtier_core_init(void)
{
	int i;
	printk(KERN_INFO "Before enable_all_cpus\n");
	bwtier_enable_all_cpus();
	printk(KERN_INFO "After periodic_cooling_enable\n");

	for (i = 0; i < NUM_BWTIER_BINS; i++) {
		access_histogram_bins[i].nr_pages = 0;
		INIT_LIST_HEAD(&(access_histogram_bins[i].pages_head));
		printk(KERN_INFO "Initted Bin %d\n", i);
	}
	printk(KERN_INFO "Initialized all Bins\n");
}

void update_pginfo(uint64_t pfn, uint64_t timestamp)
{
	int bin_id, pg_bin_id, bin_ind;
	struct page *page = pfn_to_page(pfn);
	uint64_t pg_acc_count, pg_last_cooled_ts, cool_period_ns;

	cool_period_ns = COOLING_PERIOD_MS * 1000000;
	if (!page)
		return;

	/* LOCK PAGE */
	pg_acc_count = page->access_count;
	pg_last_cooled_ts = page->last_cooled_timestamp;
	pg_bin_id = page->bin_id;
	/* UNLOCK PAGE */

	if (pg_bin_id == 0) {
		/* Page Sampled for the first time */
		page->access_count = 0;
		page->last_cooled_timestamp = timestamp;
	}

	while ((timestamp - pg_last_cooled_ts) > cool_period_ns) {
		page->access_count /= 2;
		page->last_cooled_timestamp += cool_period_ns;
	}

	page->access_count += 512;
	bin_ind = bin_index(page->access_count);
	bin_id = access_histogram_bins[bin_ind].bin_id;

	if (pg_bin_id != bin_id) {
		/* Move page to new bin */
		access_histogram_bins[pg_bin_id].nr_pages--;
		access_histogram_bins[bin_id].nr_pages++;
		list_move(&(page->bwtier_list), 
				&(access_histogram_bins[bin_id].pages_head));
	}
}

int bwtier_enable(void)
{
	int cool_secs, cool_nsecs;
	cool_secs = COOLING_PERIOD_MS / 1000;
	cool_nsecs = (COOLING_PERIOD_MS % 1000) * 1000000;
	printk(KERN_INFO "Before periodic_cooling_enable\n");
	periodic_cooling_enable(cool_secs, cool_nsecs);
	printk(KERN_INFO "After periodic_cooling_enable\n");

	ksampld_enable();
	return 0;
}

int bwtier_disable(void)
{
	ksampld_disable();
	periodic_cooling_disable();
	return 0;
}