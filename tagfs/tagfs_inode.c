/*
 * Resizable simple ram filesystem for Linux.
 *
 * Copyright (C) 2000 Linus Torvalds.
 *               2000 Transmeta Corp.
 *
 * Usage limits added by David Gibson, Linuxcare Australia.
 * This file is released under the GPL.
 */

/*
 * NOTE! This filesystem is probably most useful
 * not as a real filesystem, but as an example of
 * how virtual filesystems can be written.
 *
 * It doesn't get much simpler than this. Consider
 * that this file implements the full semantics of
 * a POSIX-compliant read-write filesystem.
 *
 * Note in particular how the filesystem does not
 * need to implement any data structures of its own
 * to keep track of the virtual data: using the VFS
 * caches is sufficient.
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/sched.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/seq_file.h>
#include <linux/dax.h>
#include <linux/hugetlb.h>
#include <linux/uio.h>
#include <linux/iomap.h>

#include "tagfs.h"
#include "tagfs_internal.h"

#define TAGFS_DEFAULT_MODE	0755

static const struct super_operations tagfs_ops;
static const struct inode_operations tagfs_dir_inode_operations;

struct inode *tagfs_get_inode(struct super_block *sb,
				const struct inode *dir, umode_t mode, dev_t dev)
{
	struct inode * inode = new_inode(sb);

	if (inode) {
		inode->i_ino = get_next_ino();
		inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
		inode->i_mapping->a_ops = &ram_aops;
		mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
		mapping_set_unevictable(inode->i_mapping);
		inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
		switch (mode & S_IFMT) {
		default:
			init_special_inode(inode, mode, dev);
			break;
		case S_IFREG:
			inode->i_op = &tagfs_file_inode_operations;
			inode->i_fop = &tagfs_file_operations;
			break;
		case S_IFDIR:
			inode->i_op = &tagfs_dir_inode_operations;
			inode->i_fop = &simple_dir_operations;

			/* directory inodes start off with i_nlink == 2 (for "." entry) */
			inc_nlink(inode);
			break;
		case S_IFLNK:
			inode->i_op = &page_symlink_inode_operations;
			inode_nohighmem(inode);
			break;
		}
	}
	return inode;
}

/*
 * File creation. Allocate an inode, and we're done..
 */
/* SMP-safe */
static int
tagfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
	    struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode * inode = tagfs_get_inode(dir->i_sb, dir, mode, dev);
	int error = -ENOSPC;

	if (inode) {
		d_instantiate(dentry, inode);
		dget(dentry);	/* Extra count - pin the dentry in core */
		error = 0;
		dir->i_mtime = dir->i_ctime = current_time(dir);
	}
	return error;
}

static int tagfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode)
{
	int retval = tagfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFDIR, 0);
	if (!retval)
		inc_nlink(dir);
	return retval;
}

static int tagfs_create(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
{
	return tagfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFREG, 0);
}

static int tagfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, const char *symname)
{
	struct inode *inode;
	int error = -ENOSPC;

	inode = tagfs_get_inode(dir->i_sb, dir, S_IFLNK|S_IRWXUGO, 0);
	if (inode) {
		int l = strlen(symname)+1;
		error = page_symlink(inode, symname, l);
		if (!error) {
			d_instantiate(dentry, inode);
			dget(dentry);
			dir->i_mtime = dir->i_ctime = current_time(dir);
		} else
			iput(inode);
	}
	return error;
}

static int tagfs_tmpfile(struct mnt_idmap *idmap,
			 struct inode *dir, struct file *file, umode_t mode)
{
	struct inode *inode;

	inode = tagfs_get_inode(dir->i_sb, dir, mode, 0);
	if (!inode)
		return -ENOSPC;
	d_tmpfile(file, inode);
	return finish_open_simple(file, 0);
}

static const struct inode_operations tagfs_dir_inode_operations = {
	.create		= tagfs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink		= simple_unlink,
	.symlink	= tagfs_symlink,
	.mkdir		= tagfs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= tagfs_mknod,
	.rename		= simple_rename,
	.tmpfile	= tagfs_tmpfile,
};

/*
 * Display the mount options in /proc/mounts.
 */
static int tagfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct tagfs_fs_info *fsi = root->d_sb->s_fs_info;

	if (fsi->mount_opts.mode != TAGFS_DEFAULT_MODE)
		seq_printf(m, ",mode=%o", fsi->mount_opts.mode);
	return 0;
}

static const struct super_operations tagfs_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= generic_delete_inode,
	.show_options	= tagfs_show_options,
};

enum tagfs_param {
	Opt_mode,
};

const struct fs_parameter_spec tagfs_fs_parameters[] = {
	fsparam_u32oct("mode",	Opt_mode),
	{}
};

static int tagfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct fs_parse_result result;
	struct tagfs_fs_info *fsi = fc->s_fs_info;
	int opt;

	opt = fs_parse(fc, tagfs_fs_parameters, param, &result);
	if (opt == -ENOPARAM) {
		opt = vfs_parse_fs_param_source(fc, param);
		if (opt != -ENOPARAM)
			return opt;
		/*
		 * We might like to report bad mount options here;
		 * but traditionally tagfs has ignored all mount options,
		 * and as it is used as a !CONFIG_SHMEM simple substitute
		 * for tmpfs, better continue to ignore other mount options.
		 */
		return 0;
	}
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_mode:
		fsi->mount_opts.mode = result.uint_32 & S_IALLUGO;
		break;
	}

	return 0;
}

static int tagfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct tagfs_fs_info *fsi = sb->s_fs_info;
	struct inode *inode;

	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_blocksize		= PAGE_SIZE;
	sb->s_blocksize_bits	= PAGE_SHIFT;
	sb->s_magic		= TAGFS_MAGIC;
	sb->s_op		= &tagfs_ops;
	sb->s_time_gran		= 1;

	inode = tagfs_get_inode(sb, NULL, S_IFDIR | fsi->mount_opts.mode, 0);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	return 0;
}

static int tagfs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, tagfs_fill_super);
}

static void tagfs_free_fc(struct fs_context *fc)
{
	kfree(fc->s_fs_info);
}

static const struct fs_context_operations tagfs_context_ops = {
	.free		= tagfs_free_fc,
	.parse_param	= tagfs_parse_param,
	.get_tree	= tagfs_get_tree,
};

int tagfs_init_fs_context(struct fs_context *fc)
{
	struct tagfs_fs_info *fsi;

	fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
	if (!fsi)
		return -ENOMEM;

	mutex_init(&fsi->fsi_mutex);
	fsi->mount_opts.mode = TAGFS_DEFAULT_MODE;
	fc->s_fs_info = fsi;
	fc->ops = &tagfs_context_ops;
	return 0;
}

static void tagfs_kill_sb(struct super_block *sb)
{
	struct tagfs_fs_info *fsi = sb->s_fs_info;

	mutex_destroy(&fsi->fsi_mutex);
	if (fsi->bdevp)
		blkdev_put(fsi->bdevp, tagfs_blkdev_mode);
	if (fsi->dax_devp)
		fs_put_dax(fsi->dax_devp, fsi);
	kfree(sb->s_fs_info);
	kill_litter_super(sb);
}

static struct file_system_type tagfs_fs_type = {
	.name		= "tagfs",
	.init_fs_context = tagfs_init_fs_context,
	.parameters	= tagfs_fs_parameters,
	.kill_sb	= tagfs_kill_sb,
	.fs_flags	= FS_USERNS_MOUNT,
};

static int __init init_tagfs_fs(void)
{
	printk("%s\n", __func__);
	/* See what the different log levels do */
	printk(KERN_DEBUG    "%s: KERN_DEBUG \n", __func__);
	printk(KERN_INFO     "%s: KERN_INFO \n", __func__);
	printk(KERN_NOTICE   "%s: KERN_NOTICE \n", __func__);
	printk(KERN_WARNING  "%s: KERN_WARNING \n", __func__);
	printk(KERN_ERR      "%s: KERN_ERR \n", __func__);
#if 0
	printk(KERN_CRIT     "%s: KERN_CRIT \n", __func__);
	printk(KERN_ALERT    "%s: KERN_ALERT \n", __func__);
	printk(KERN_EMERG    "%s: KERN_EMERG \n", __func__);
#endif
	return register_filesystem(&tagfs_fs_type);
}

void
__exit tagfs_exit(void)
{
	printk("%s\n", __func__);
	unregister_filesystem(&tagfs_fs_type);
	printk("%s: unregistered\n", __func__);
}

fs_initcall(init_tagfs_fs);
module_exit(tagfs_exit);