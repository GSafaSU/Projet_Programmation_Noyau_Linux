#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h> 

#include "ouichefs.h"
#include "bitmap.h"

static struct kobject *ouichefs_root_kobj;


struct ouichefs_kobj {
	struct kobject kobj;
	struct super_block *sb;
};

#define to_ouichefs_kobj(x) \
	container_of(x, struct ouichefs_kobj, kobj)


static void scan_inodes(struct super_block *sb,
			uint32_t *committed, uint32_t *total_ext,
			uint32_t *files, uint64_t *max_size)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	uint32_t ino;

	*committed = 0;
	*total_ext = 0;
	*files     = 0;
	*max_size  = 0;

	for (ino = 1; ino < sbi->nr_inodes; ino++) {
		struct inode *inode = ilookup(sb, ino);
		struct ouichefs_inode_info *ci;
		struct buffer_head *bh;
		struct ouichefs_file_index_block *index;
		int i;

		if (!inode)
			continue;
		if (!S_ISREG(inode->i_mode)) {
			iput(inode);
			continue;
		}

		ci  = OUICHEFS_INODE(inode);
		bh  = sb_bread(sb, ci->index_block);
		if (!bh) {
			iput(inode);
			continue;
		}

		index = (struct ouichefs_file_index_block *)bh->b_data;
		(*files)++;
		(*committed)++; //L'index bloc

		for (i = 0; i < OUICHEFS_MAX_EXTENTS; i++) {
			uint32_t c = le32_to_cpu(index->extents[i].count);

			if (c == 0)
				break;
			(*total_ext)++;
			(*committed) += c;
		}

		if ((uint64_t)inode->i_size > *max_size)
			*max_size = inode->i_size;

		brelse(bh);
		iput(inode);
	}
}

static uint32_t count_reserved(struct super_block *sb)
{
	struct inode *inode;
	uint32_t reserved = 0;

	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);

		reserved += ci->i_reserved_count;
	}
	spin_unlock(&sb->s_inode_list_lock);

	return reserved;
}



static ssize_t free_blocks_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct super_block *sb = to_ouichefs_kobj(kobj)->sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	return sprintf(buf, "%u\n", sbi->nr_free_blocks);
}

static ssize_t committed_blocks_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	struct super_block *sb = to_ouichefs_kobj(kobj)->sb;
	uint32_t committed, total_ext, files;
	uint64_t max_size;

	scan_inodes(sb, &committed, &total_ext, &files, &max_size);
	return sprintf(buf, "%u\n", committed);
}

static ssize_t reserved_blocks_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	struct super_block *sb = to_ouichefs_kobj(kobj)->sb;

	return sprintf(buf, "%u\n", count_reserved(sb));
}

static ssize_t files_show(struct kobject *kobj,
			  struct kobj_attribute *attr, char *buf)
{
	struct super_block *sb = to_ouichefs_kobj(kobj)->sb;
	uint32_t committed, total_ext, files;
	uint64_t max_size;

	scan_inodes(sb, &committed, &total_ext, &files, &max_size);
	return sprintf(buf, "%u\n", files);
}

static ssize_t total_extents_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	struct super_block *sb = to_ouichefs_kobj(kobj)->sb;
	uint32_t committed, total_ext, files;
	uint64_t max_size;

	scan_inodes(sb, &committed, &total_ext, &files, &max_size);
	return sprintf(buf, "%u\n", total_ext);
}

static ssize_t avg_extent_size_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	struct super_block *sb = to_ouichefs_kobj(kobj)->sb;
	uint32_t committed, total_ext, files;
	uint64_t max_size;
	uint32_t avg;

	scan_inodes(sb, &committed, &total_ext, &files, &max_size);
	avg = total_ext ? (committed * 100) / total_ext : 0;
	return sprintf(buf, "%u\n", avg);
}

static ssize_t max_file_size_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	struct super_block *sb = to_ouichefs_kobj(kobj)->sb;
	uint32_t committed, total_ext, files;
	uint64_t max_size;

	scan_inodes(sb, &committed, &total_ext, &files, &max_size);
	return sprintf(buf, "%llu\n", max_size);
}

static ssize_t fragmentation_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	struct super_block *sb = to_ouichefs_kobj(kobj)->sb;
	uint32_t committed, total_ext, files;
	uint64_t max_size;
	uint32_t frag;

	scan_inodes(sb, &committed, &total_ext, &files, &max_size);
	frag = files ? (total_ext * 100) / files : 0;
	return sprintf(buf, "%u\n", frag);
}

static ssize_t reservation_size_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", reservation_size);
}

static ssize_t reservation_size_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	uint32_t val;

	if (kstrtou32(buf, 10, &val) || val == 0)
		return -EINVAL;
	reservation_size = val;
	return count;
}

static ssize_t gc_runs_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	struct super_block *sb = to_ouichefs_kobj(kobj)->sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	return sprintf(buf, "%u\n", sbi->gc_runs);
}



static struct kobj_attribute free_blocks_attr = __ATTR(free_blocks, 0444, free_blocks_show, NULL);
static struct kobj_attribute committed_blocks_attr = __ATTR(committed_blocks, 0444, committed_blocks_show, NULL);
static struct kobj_attribute reserved_blocks_attr = __ATTR(reserved_blocks, 0444, reserved_blocks_show, NULL);
static struct kobj_attribute files_attr = __ATTR(files, 0444, files_show, NULL);
static struct kobj_attribute total_extents_attr = __ATTR(total_extents, 0444, total_extents_show, NULL);
static struct kobj_attribute avg_extent_size_attr = __ATTR(avg_extent_size, 0444, avg_extent_size_show, NULL);
static struct kobj_attribute max_file_size_attr = __ATTR(max_file_size, 0444, max_file_size_show, NULL);
static struct kobj_attribute fragmentation_attr = __ATTR(fragmentation, 0444, fragmentation_show, NULL);
static struct kobj_attribute reservation_size_attr = __ATTR(reservation_size, 0644, reservation_size_show, reservation_size_store);
static struct kobj_attribute gc_runs_attr = __ATTR(gc_runs, 0444, gc_runs_show, NULL);

static struct attribute *ouichefs_attrs[] = {
	&free_blocks_attr.attr,
	&committed_blocks_attr.attr,
	&reserved_blocks_attr.attr,
	&files_attr.attr,
	&total_extents_attr.attr,
	&avg_extent_size_attr.attr,
	&max_file_size_attr.attr,
	&fragmentation_attr.attr,
	&reservation_size_attr.attr,
	&gc_runs_attr.attr,
	NULL,
};

ATTRIBUTE_GROUPS(ouichefs);


static void ouichefs_kobj_release(struct kobject *kobj)
{
	kfree(to_ouichefs_kobj(kobj));
}

static struct kobj_type ouichefs_ktype = {
	.release        = ouichefs_kobj_release,
	.sysfs_ops      = &kobj_sysfs_ops,
	.default_groups = ouichefs_groups,
};


int ouichefs_sysfs_init(struct super_block *sb, const char *devname)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_kobj *okobj;
	int ret;

	pr_info("sysfs_init called for %s\n", devname);

	if (!ouichefs_root_kobj) {
		ouichefs_root_kobj =
			kobject_create_and_add("ouichefs", kernel_kobj);
		if (!ouichefs_root_kobj) {
			pr_err("failed to create root kobject\n");
			return -ENOMEM;
		}
		pr_info("root kobject created\n");
	}

	okobj = kzalloc(sizeof(*okobj), GFP_KERNEL);
	if (!okobj) {
		pr_err("kzalloc failed\n");
		return -ENOMEM;
	}

	okobj->sb = sb;

	ret = kobject_init_and_add(&okobj->kobj, &ouichefs_ktype,
				   ouichefs_root_kobj, "%s", devname);
	if (ret) {
		pr_err("kobject_init_and_add failed: %d\n", ret);
		kobject_put(&okobj->kobj);
		return ret;
	}

	pr_info("kobject added at /sys/ouichefs/%s\n", devname);

	sbi->s_kobj = &okobj->kobj;
	kobject_uevent(&okobj->kobj, KOBJ_ADD);

	return 0;
}

void ouichefs_sysfs_exit(struct super_block *sb)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	if (sbi && sbi->s_kobj) {
		kobject_put(sbi->s_kobj);
		sbi->s_kobj = NULL;
	}

	if (ouichefs_root_kobj) {
		kobject_put(ouichefs_root_kobj);
		ouichefs_root_kobj = NULL;
	}
}