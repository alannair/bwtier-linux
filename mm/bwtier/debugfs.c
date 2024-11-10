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
	struct cpumask mask;
	char kbuf[512];
	int len;

	bwtier_get_cpu_bitmap(&mask);
	len = scnprintf(kbuf, 511, "%*pbl\n", cpumask_pr_args(&mask));

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t bwtier_debugfs_cpus_write(struct file *file,
		const char __user *buf, size_t count, loff_t *ppos)
{
	struct cpumask mask;
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

	ret = cpulist_parse(kbuf, &mask);
	bwtier_set_cpu_bitmap(&mask);

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

static int __init bwtier_debugfs_init(void)
{
	bwtier_sysfs_root = debugfs_create_dir("bwtier", NULL);
	if (!bwtier_sysfs_root)
		return -ENOMEM;

	debugfs_create_file("status", 0600, bwtier_sysfs_root, 
		NULL, &bwtier_debugfs_status_fops);
	debugfs_create_file("cpus", 0600, bwtier_sysfs_root, 
		NULL, &bwtier_debugfs_cpus_fops);

	ksampld_init();
	bwtier_core_init();

	return 0;
}
subsys_initcall(bwtier_debugfs_init);	