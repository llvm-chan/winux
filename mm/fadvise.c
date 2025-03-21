// SPDX-License-Identifier: GPL-2.0
/*
 * mm/fadvise.c
 *
 * Copyright (C) 2002, Linus Torvalds
 *
 * 11Jan2003	Andrew Morton
 *		Initial version.
 */

#include <winux/kernel.h>
#include <winux/file.h>
#include <winux/fs.h>
#include <winux/mm.h>
#include <winux/pagemap.h>
#include <winux/backing-dev.h>
#include <winux/fadvise.h>
#include <winux/writeback.h>
#include <winux/syscalls.h>
#include <winux/swap.h>

#include <asm/unistd.h>

#include "internal.h"

/*
 * POSIX_FADV_WILLNEED could set PG_Referenced, and POSIX_FADV_NOREUSE could
 * deactivate the pages and clear PG_Referenced.
 */

int generic_fadvise(struct file *file, loff_t offset, loff_t len, int advice)
{
	struct inode *inode;
	struct address_space *mapping;
	struct backing_dev_info *bdi;
	loff_t endbyte;			/* inclusive */
	pgoff_t start_index;
	pgoff_t end_index;
	unsigned long nrpages;

	inode = file_inode(file);
	if (S_ISFIFO(inode->i_mode))
		return -ESPIPE;

	mapping = file->f_mapping;
	if (!mapping || len < 0)
		return -EINVAL;

	bdi = inode_to_bdi(mapping->host);

	if (IS_DAX(inode) || (bdi == &noop_backing_dev_info)) {
		switch (advice) {
		case POSIX_FADV_NORMAL:
		case POSIX_FADV_RANDOM:
		case POSIX_FADV_SEQUENTIAL:
		case POSIX_FADV_WILLNEED:
		case POSIX_FADV_NOREUSE:
		case POSIX_FADV_DONTNEED:
			/* no bad return value, but ignore advice */
			break;
		default:
			return -EINVAL;
		}
		return 0;
	}

	/*
	 * Careful about overflows. Len == 0 means "as much as possible".  Use
	 * unsigned math because signed overflows are undefined and UBSan
	 * complains.
	 */
	endbyte = (u64)offset + (u64)len;
	if (!len || endbyte < len)
		endbyte = LLONG_MAX;
	else
		endbyte--;		/* inclusive */

	switch (advice) {
	case POSIX_FADV_NORMAL:
		file->f_ra.ra_pages = bdi->ra_pages;
		spin_lock(&file->f_lock);
		file->f_mode &= ~(FMODE_RANDOM | FMODE_NOREUSE);
		spin_unlock(&file->f_lock);
		break;
	case POSIX_FADV_RANDOM:
		spin_lock(&file->f_lock);
		file->f_mode |= FMODE_RANDOM;
		spin_unlock(&file->f_lock);
		break;
	case POSIX_FADV_SEQUENTIAL:
		file->f_ra.ra_pages = bdi->ra_pages * 2;
		spin_lock(&file->f_lock);
		file->f_mode &= ~FMODE_RANDOM;
		spin_unlock(&file->f_lock);
		break;
	case POSIX_FADV_WILLNEED:
		/* First and last PARTIAL page! */
		start_index = offset >> PAGE_SHIFT;
		end_index = endbyte >> PAGE_SHIFT;

		/* Careful about overflow on the "+1" */
		nrpages = end_index - start_index + 1;
		if (!nrpages)
			nrpages = ~0UL;

		force_page_cache_readahead(mapping, file, start_index, nrpages);
		break;
	case POSIX_FADV_NOREUSE:
		spin_lock(&file->f_lock);
		file->f_mode |= FMODE_NOREUSE;
		spin_unlock(&file->f_lock);
		break;
	case POSIX_FADV_DONTNEED:
		__filemap_fdatawrite_range(mapping, offset, endbyte,
					   WB_SYNC_NONE);

		/*
		 * First and last FULL page! Partial pages are deliberately
		 * preserved on the expectation that it is better to preserve
		 * needed memory than to discard unneeded memory.
		 */
		start_index = (offset+(PAGE_SIZE-1)) >> PAGE_SHIFT;
		end_index = (endbyte >> PAGE_SHIFT);
		/*
		 * The page at end_index will be inclusively discarded according
		 * by invalidate_mapping_pages(), so subtracting 1 from
		 * end_index means we will skip the last page.  But if endbyte
		 * is page aligned or is at the end of file, we should not skip
		 * that page - discarding the last page is safe enough.
		 */
		if ((endbyte & ~PAGE_MASK) != ~PAGE_MASK &&
				endbyte != inode->i_size - 1) {
			/* First page is tricky as 0 - 1 = -1, but pgoff_t
			 * is unsigned, so the end_index >= start_index
			 * check below would be true and we'll discard the whole
			 * file cache which is not what was asked.
			 */
			if (end_index == 0)
				break;

			end_index--;
		}

		if (end_index >= start_index) {
			unsigned long nr_failed = 0;

			/*
			 * It's common to FADV_DONTNEED right after
			 * the read or write that instantiates the
			 * pages, in which case there will be some
			 * sitting on the local LRU cache. Try to
			 * avoid the expensive remote drain and the
			 * second cache tree walk below by flushing
			 * them out right away.
			 */
			lru_add_drain();

			mapping_try_invalidate(mapping, start_index, end_index,
					&nr_failed);

			/*
			 * The failures may be due to the folio being
			 * in the LRU cache of a remote CPU. Drain all
			 * caches and try again.
			 */
			if (nr_failed) {
				lru_add_drain_all();
				invalidate_mapping_pages(mapping, start_index,
						end_index);
			}
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}
EXPORT_SYMBOL(generic_fadvise);

int vfs_fadvise(struct file *file, loff_t offset, loff_t len, int advice)
{
	if (file->f_op->fadvise)
		return file->f_op->fadvise(file, offset, len, advice);

	return generic_fadvise(file, offset, len, advice);
}
EXPORT_SYMBOL(vfs_fadvise);

#ifdef CONFIG_ADVISE_SYSCALLS

int ksys_fadvise64_64(int fd, loff_t offset, loff_t len, int advice)
{
	CLASS(fd, f)(fd);

	if (fd_empty(f))
		return -EBADF;

	return vfs_fadvise(fd_file(f), offset, len, advice);
}

SYSCALL_DEFINE4(fadvise64_64, int, fd, loff_t, offset, loff_t, len, int, advice)
{
	return ksys_fadvise64_64(fd, offset, len, advice);
}

#ifdef __ARCH_WANT_SYS_FADVISE64

SYSCALL_DEFINE4(fadvise64, int, fd, loff_t, offset, size_t, len, int, advice)
{
	return ksys_fadvise64_64(fd, offset, len, advice);
}

#endif

#if defined(CONFIG_COMPAT) && defined(__ARCH_WANT_COMPAT_FADVISE64_64)

COMPAT_SYSCALL_DEFINE6(fadvise64_64, int, fd, compat_arg_u64_dual(offset),
		       compat_arg_u64_dual(len), int, advice)
{
	return ksys_fadvise64_64(fd, compat_arg_u64_glue(offset),
				 compat_arg_u64_glue(len), advice);
}

#endif
#endif
