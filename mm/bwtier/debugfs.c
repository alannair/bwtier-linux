// SPDX-License-Identifier: GPL-2.0
/*
 * BWTIER sysfs Interface
 *
 * Copyright (c) 2024 Alan Nair <alannair1000@gmail.com>
 */

#include <linux/debugfs.h>
#include <linux/bwtier.h>

struct dentry *bwtier_sysfs_root;

static int bwtier_debugfs_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return nonseekable_open(inode, file);
}

static ssize_t bwtier_debugfs_status_read(struct file *file,
		char __user *buf, size_t count, loff_t *ppos)
{
	char kbuf[5];
	int len;

	len = scnprintf(kbuf, 5, bwtier_status() ? "on\n" : "off\n");

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t bwtier_debugfs_status_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	ssize_t ret, ret2;
	char *kbuf;

	if (*ppos)
		return -EINVAL;

	kbuf = kmalloc(count + 1, GFP_KERNEL | __GFP_NOWARN);
	if (!kbuf)
		return -ENOMEM;

	ret2 = simple_write_to_buffer(kbuf, count + 1, ppos, buf, count);
	if (ret2 != count) {
		kfree(kbuf);
		return -EIO;
	}
	kbuf[ret2] = '\0';

	/* Remove white space */
	if (sscanf(kbuf, "%s", kbuf) != 1) {
		kfree(kbuf);
		return -EINVAL;
	}

	if (!strncmp(kbuf, "on", count)) {
		ret = bwtier_enable();
	} else if (!strncmp(kbuf, "off", count)) {
		ret = bwtier_disable();
	} else {
		ret = -EINVAL;
	}

	if (!ret)
		ret = count;
	kfree(kbuf);
	return ret;
}

static ssize_t bwtier_debugfs_cpus_read(struct file *file,
		char __user *buf, size_t count, loff_t *ppos)
{
	struct cpumask *mask;
	char *kbuf;
	int len;
	ssize_t ret;

	mask = kzalloc(sizeof(struct cpumask), GFP_KERNEL | __GFP_NOWARN);
	if (!mask) {
		ret = -ENOMEM;
		goto out_mask;
	}

	kbuf = kzalloc(512, GFP_KERNEL | __GFP_NOWARN);
	if (!kbuf) {
		ret = -ENOMEM;
		goto out_buf;
	}

	bwtier_get_cpu_bitmap(mask);
	len = scnprintf(kbuf, 511, "%*pbl\n", cpumask_pr_args(mask));

	ret =  simple_read_from_buffer(buf, count, ppos, kbuf, len);
out_buf:
	kfree(kbuf);
out_mask:
	kfree(mask);
	return ret;
}

static ssize_t bwtier_debugfs_cpus_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	struct cpumask *mask;
	ssize_t ret, ret2;
	char *kbuf;

	if (*ppos)
		return -EINVAL;

	mask = kzalloc(sizeof(struct cpumask), GFP_KERNEL | __GFP_NOWARN);
	if (!mask) {
		ret = -ENOMEM;
		goto out_mask;
	}

	kbuf = kzalloc(count + 1, GFP_KERNEL | __GFP_NOWARN);
	if (!kbuf) {
		ret = -ENOMEM;
		goto out_buf;
	}

	ret2 = simple_write_to_buffer(kbuf, count + 1, ppos, buf, count);
	if (ret2 != count) {
		ret = -EIO;
		goto out_buf;
	}
	kbuf[ret2] = '\0';

	/* Remove white space */
	if (sscanf(kbuf, "%s", kbuf) != 1) {
		ret = -EINVAL;
		goto out_buf;
	}

	ret = cpulist_parse(kbuf, mask);
	bwtier_set_cpu_bitmap(mask);

	if (!ret)
		ret = count;

out_buf:
	kfree(kbuf);
out_mask:
	kfree(mask);
	return ret;
}

static ssize_t bwtier_debugfs_ppc_read(struct file *file,
		char __user *buf, size_t count, loff_t *ppos)
{
	char kbuf[8];
	int len, pages;

	pages = bwtier_get_pages_per_cpuevent();
	len = scnprintf(kbuf, 8, "%d\n", pages);

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t bwtier_debugfs_ppc_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	int pages;
	ssize_t ret;
	char kbuf[8];

	if (*ppos)
		return -EINVAL;

	ret = simple_write_to_buffer(kbuf, count + 1, ppos, buf, count);
	if (ret != count) {
		return -EIO;
	}
	kbuf[ret] = '\0';

	/* Remove white space */
	if (sscanf(kbuf, "%d", &pages) != 1) {
		return -EINVAL;
	}

	ret = bwtier_set_pages_per_cpuevent(pages);

	if (!ret)
		ret = count;

	return ret;
}

static ssize_t bwtier_debugfs_freq_read(struct file *file,
		char __user *buf, size_t count, loff_t *ppos)
{
	char kbuf[8];
	int len, freq;

	freq = bwtier_get_pebsfreq();
	len = scnprintf(kbuf, 8, "%d\n", freq);

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t bwtier_debugfs_freq_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	int freq;
	ssize_t ret;
	char kbuf[8];

	if (*ppos)
		return -EINVAL;

	ret = simple_write_to_buffer(kbuf, count + 1, ppos, buf, count);
	if (ret != count) {
		return -EIO;
	}
	kbuf[ret] = '\0';

	/* Remove white space */
	if (sscanf(kbuf, "%d", &freq) != 1) {
		return -EINVAL;
	}

	ret = bwtier_set_pebsfreq(freq);

	if (!ret)
		ret = count;

	return ret;
}

static int bwtier_debugfs_pid_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	if ((file->f_mode & FMODE_WRITE) &&
	    (file->f_flags & O_TRUNC))
		clear_bwtier_pids();

	return nonseekable_open(inode, file);
}

static ssize_t bwtier_debugfs_pid_read(struct file *file,
		char __user *buf, size_t count, loff_t *ppos)
{
	char *kbuf;
	int len, npids, i, ret;
	pid_t *pids;

	kbuf = kzalloc(512, GFP_KERNEL | __GFP_NOWARN);
	if (!kbuf)
		return -ENOMEM;

	pids = get_pid_list(&npids);
	for (i = 0; i < npids; i++) {
		len += scnprintf(kbuf + len, 511 - len, "%d\n", pids[i]);
	}

	ret = simple_read_from_buffer(buf, count, ppos, kbuf, len);
	kfree(kbuf);
	return ret;
}

static ssize_t bwtier_debugfs_pid_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	ssize_t ret;
	char *kbuf;
	pid_t pid;

	if (*ppos)
		return -EINVAL;

	kbuf = kzalloc(count + 1, GFP_KERNEL | __GFP_NOWARN);
	if (!kbuf)
		return -ENOMEM;

	ret = simple_write_to_buffer(kbuf, count + 1, ppos, buf, count);
	if (ret != count) {
		kfree(kbuf);
		return -EIO;
	}
	kbuf[ret] = '\0';

	if (sscanf(kbuf, "%d", &pid) == 1) {
		bwtier_set_pid(pid);
	}

	kfree(kbuf);
	return count;
}

static ssize_t bwtier_debugfs_stats_ms_read(struct file *file,
		char __user *buf, size_t count, loff_t *ppos)
{
	char kbuf[8];
	int len;

	len = scnprintf(kbuf, 8, "%d\n", bwtier_stats_ms());

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t bwtier_debugfs_stats_ms_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	ssize_t ret, ret2;
	char *kbuf;
	int ms;

	if (*ppos)
		return -EINVAL;

	kbuf = kzalloc(count + 1, GFP_KERNEL | __GFP_NOWARN);
	if (!kbuf)
		return -ENOMEM;

	ret2 = simple_write_to_buffer(kbuf, count + 1, ppos, buf, count);
	if (ret2 != count) {
		ret = -EIO;
		goto out_buf;
	}
	kbuf[ret2] = '\0';

	/* Remove white space */
	if (sscanf(kbuf, "%d", &ms) != 1) {
		ret = -EINVAL;
		goto out_buf;
	}

	set_bwtier_stats_ms(ms);

	if (!ret)
		ret = count;

out_buf:
	kfree(kbuf);
	return ret;
}

static ssize_t bwtier_debugfs_cool_ms_read(struct file *file,
		char __user *buf, size_t count, loff_t *ppos)
{
	char kbuf[8];
	int len;

	len = scnprintf(kbuf, 8, "%d\n", bwtier_cool_ms());

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t bwtier_debugfs_cool_ms_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	ssize_t ret, ret2;
	char *kbuf;
	int ms;

	if (*ppos)
		return -EINVAL;

	kbuf = kmalloc(count + 1, GFP_KERNEL | __GFP_NOWARN);
	if (!kbuf)
		return -ENOMEM;

	ret2 = simple_write_to_buffer(kbuf, count + 1, ppos, buf, count);
	if (ret2 != count) {
		kfree(kbuf);
		return -EIO;
	}
	kbuf[ret2] = '\0';

	/* Remove white space */
	if (sscanf(kbuf, "%d", &ms) != 1) {
		kfree(kbuf);
		return -EINVAL;
	}

	set_bwtier_cool_ms(ms);

	if (!ret)
		ret = count;
	kfree(kbuf);
	return ret;
}

static const struct file_operations bwtier_debugfs_status_fops = {
	.open = bwtier_debugfs_open,
	.read = bwtier_debugfs_status_read,
	.write = bwtier_debugfs_status_write,
};

static const struct file_operations bwtier_debugfs_cpus_fops = {
	.open = bwtier_debugfs_open,
	.read = bwtier_debugfs_cpus_read,
	.write = bwtier_debugfs_cpus_write,
};

static const struct file_operations bwtier_debugfs_ppc_fops = {
	.open = bwtier_debugfs_open,
	.read = bwtier_debugfs_ppc_read,
	.write = bwtier_debugfs_ppc_write,
};

static const struct file_operations bwtier_debugfs_freq_fops = {
	.open = bwtier_debugfs_open,
	.read = bwtier_debugfs_freq_read,
	.write = bwtier_debugfs_freq_write,
};

static const struct file_operations bwtier_debugfs_pid_fops = {
	.open = bwtier_debugfs_pid_open,
	.read = bwtier_debugfs_pid_read,
	.write = bwtier_debugfs_pid_write,
};

static const struct file_operations bwtier_debugfs_stats_ms_fops = {
	.open = bwtier_debugfs_open,
	.read = bwtier_debugfs_stats_ms_read,
	.write = bwtier_debugfs_stats_ms_write,
};

static const struct file_operations bwtier_debugfs_cool_ms_fops = {
	.open = bwtier_debugfs_open,
	.read = bwtier_debugfs_cool_ms_read,
	.write = bwtier_debugfs_cool_ms_write,
};

static int __init bwtier_debugfs_init(void)
{
	bwtier_sysfs_root = debugfs_create_dir("bwtier", NULL);
	if (!bwtier_sysfs_root)
		return -ENOMEM;

	debugfs_create_file("status", 0600, bwtier_sysfs_root, 
		NULL, &bwtier_debugfs_status_fops);
	debugfs_create_file("cpus", 0600, bwtier_sysfs_root, 
		NULL, &bwtier_debugfs_cpus_fops);
	debugfs_create_file("pages_per_cpuevent", 0600,
		bwtier_sysfs_root, NULL, &bwtier_debugfs_ppc_fops);
	debugfs_create_file("sampling_freq", 0600,
		bwtier_sysfs_root, NULL, &bwtier_debugfs_freq_fops);
	debugfs_create_file("pidfilter", 0600,
		bwtier_sysfs_root, NULL, &bwtier_debugfs_pid_fops);
	debugfs_create_file("stats_ms", 0600,
		bwtier_sysfs_root, NULL, &bwtier_debugfs_stats_ms_fops);
	debugfs_create_file("cool_ms", 0600,
		bwtier_sysfs_root, NULL, &bwtier_debugfs_cool_ms_fops);

	bwtier_core_init();
	ksampld_init();

	return 0;
}
subsys_initcall(bwtier_debugfs_init);	