// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2025. Huawei Technologies Co., Ltd */

#include "internal.h"

#include <linux/pagemap.h>
#include <linux/uio.h>
#include <linux/types.h>

static struct mfs_file_info *mfs_file_info_alloc(struct file *lower, struct file *cache)
{
	struct mfs_file_info  *info = kzalloc(sizeof(struct mfs_file_info), GFP_KERNEL);

	if (unlikely(!info))
		return NULL;

	info->lower = lower;
	info->cache = cache;
	return info;
}

static void mfs_file_info_free(struct mfs_file_info *info)
{
	fput(info->cache);
	fput(info->lower);
	kfree(info);
}

static int mfs_open(struct inode *inode, struct file *file)
{
	struct dentry *dentry = file_dentry(file);
	struct mfs_sb_info *sbi = MFS_SB(inode->i_sb);
	struct path lpath, cpath;
	struct file *lfile, *cfile;
	int flags = file->f_flags | MFS_OPEN_FLAGS;
	struct mfs_file_info *file_info;
	int err = 0;

	mfs_get_path(dentry, &lpath, &cpath);
	lfile = dentry_open(&lpath, flags, current_cred());
	if (IS_ERR(lfile)) {
		err = PTR_ERR(lfile);
		goto put_path;
	}

	cfile = dentry_open(&cpath, flags, current_cred());
	if (IS_ERR(cfile)) {
		err = PTR_ERR(cfile);
		goto lfput;
	}

	if (support_event(sbi))
		/* close the default readahead */
		cfile->f_mode |= FMODE_RANDOM;
	file_info = mfs_file_info_alloc(lfile, cfile);
	if (!file_info) {
		err = -ENOMEM;
		goto cfput;
	}

	file->private_data = file_info;
	goto put_path;
cfput:
	fput(cfile);
lfput:
	fput(lfile);
put_path:
	mfs_put_path(&lpath, &cpath);
	return err;
}

static int mfs_release(struct inode *inode, struct file *file)
{
	mfs_file_info_free(file->private_data);
	return 0;
}

static loff_t mfs_llseek(struct file *file, loff_t offset, int whence)
{
	struct mfs_file_info *file_info = file->private_data;
	struct inode *inode = file_inode(file);
	struct file *lfile, *cfile;
	loff_t ret;

	if (offset == 0) {
		if (whence == SEEK_CUR)
			return file->f_pos;

		if (whence == SEEK_SET)
			return vfs_setpos(file, 0, 0);
	}

	lfile = file_info->lower;
	cfile = file_info->cache;

	mfs_inode_lock(inode);
	lfile->f_pos = file->f_pos;
	ret = vfs_llseek(lfile, offset, whence);
	if (ret < 0)
		goto out;

	cfile->f_pos = file->f_pos;
	ret = vfs_llseek(cfile, offset, whence);
	if (ret < 0)
		goto out;

	file->f_pos = lfile->f_pos;
out:
	mfs_inode_unlock(inode);
	return ret;
}

static int mfs_flush(struct file *file, fl_owner_t id)
{
	struct mfs_file_info *file_info = file->private_data;
	struct file *cfile;
	int err = 0;

	cfile = file_info->cache;
	if (cfile->f_op->flush)
		err = cfile->f_op->flush(cfile, id);

	return err;
}

static int mfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct file *lfile;
	struct mfs_file_info *file_info = file->private_data;

	lfile = file_info->lower;
	return iterate_dir(lfile, ctx);
}

static ssize_t mfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *cfile, *file = iocb->ki_filp;
	struct mfs_file_info *fi = file->private_data;
	ssize_t rsize;

	if (!iov_iter_count(to))
		return 0;

	cfile = fi->cache;
	if (!cfile->f_op->read_iter)
		return -EINVAL;

	(void)get_file(cfile);

	iocb->ki_filp = cfile;
	rsize = cfile->f_op->read_iter(iocb, to);
	iocb->ki_filp = file;
	fput(cfile);
	return rsize;
}

static int mfs_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct mfs_file_info *fi = file->private_data;
	struct file *cfile = fi->cache;
	int err;

	if (!cfile->f_op->mmap)
		return -ENODEV;

	(void)get_file(cfile);
	vma->vm_file = cfile;
	err = call_mmap(vma->vm_file, vma);
	vma->vm_file = file;
	fput(cfile);
	if (err)
		return err;

	fi->cache_vm_ops = vma->vm_ops;
	vma->vm_ops = &mfs_file_vm_ops;

	return 0;
}

static vm_fault_t mfs_filemap_fault(struct vm_fault *vmf)
{
	struct file *cfile, *file = vmf->vma->vm_file;
	struct mfs_file_info *fi = file->private_data;
	const struct vm_operations_struct *cvm_ops;
	struct vm_area_struct cvma, *vma, **vma_;
	vm_fault_t ret;

	vma = vmf->vma;
	memcpy(&cvma, vma, sizeof(struct vm_area_struct));
	cfile = fi->cache;
	cvm_ops = fi->cache_vm_ops;
	cvma.vm_file = cfile;

	if (unlikely(!cvm_ops->fault))
		return VM_FAULT_SIGBUS;

	(void)get_file(cfile);

	/*
	 * Dealing fault in mfs will call cachefile's fault eventually,
	 * hence we will change vmf->vma->vm_file to cachefile.
	 * When faulting concurrently, changing vmf->vma->vm_file is
	 * visible to other threads. Hence we use cvma to narrow the
	 * visibility. vmf->vma is const, so we use **vma_ to change.
	 */
	vma_ = (struct vm_area_struct **)&vmf->vma;
	*vma_ = &cvma;
	ret = cvm_ops->fault(vmf);
	*vma_ = vma;
	fput(cfile);
	return ret;
}

vm_fault_t mfs_filemap_map_pages(struct vm_fault *vmf,
					pgoff_t start_pgoff, pgoff_t end_pgoff)
{
	struct file *cfile, *file = vmf->vma->vm_file;
	struct mfs_file_info *fi = file->private_data;
	const struct vm_operations_struct *cvm_ops;
	struct vm_area_struct cvma, *vma, **vma_;
	vm_fault_t ret;

	vma = vmf->vma;
	memcpy(&cvma, vma, sizeof(struct vm_area_struct));
	cfile = fi->cache;
	cvm_ops = fi->cache_vm_ops;
	cvma.vm_file = cfile;
	(void)get_file(cfile);
	if (unlikely(!cvm_ops->map_pages)) {
		fput(cfile);
		return 0;
	}

	vma_ = (struct vm_area_struct **)&vmf->vma;
	*vma_ = &cvma;
	ret = cvm_ops->map_pages(vmf, start_pgoff, end_pgoff);
	*vma_ = vma;
	fput(cfile);

	return ret;
}

static int mfs_fadvise(struct file *file, loff_t offset, loff_t len, int advice)
{
	struct inode *inode = file_inode(file);
	struct mfs_sb_info *sbi = MFS_SB(inode->i_sb);
	struct mfs_file_info *fi;
	struct file *cfile;
	int ret;

	/* avoid trigger readahead in event mode */
	if (support_event(sbi))
		return generic_fadvise(file, offset, len, advice);

	fi = file->private_data;
	cfile = fi->cache;
	(void)get_file(cfile);

	ret = vfs_fadvise(cfile, offset, len, advice);
	fput(cfile);

	return ret;
}

const struct file_operations mfs_dir_fops = {
	.open		= mfs_open,
	.iterate_shared	= mfs_readdir,
	.release	= mfs_release,
};

const struct file_operations mfs_file_fops = {
	.open		= mfs_open,
	.release	= mfs_release,
	.llseek		= mfs_llseek,
	.read_iter	= mfs_read_iter,
	.flush		= mfs_flush,
	.mmap		= mfs_file_mmap,
	.fadvise	= mfs_fadvise,
};

const struct vm_operations_struct mfs_file_vm_ops = {
	.fault		= mfs_filemap_fault,
	.map_pages	= mfs_filemap_map_pages,
};

const struct address_space_operations mfs_aops = {
	.direct_IO	= noop_direct_IO,
};
