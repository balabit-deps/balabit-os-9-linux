/*
 *  linux/drivers/block/loop.c
 *
 *  Written by Theodore Ts'o, 3/29/93
 *
 * Copyright 1993 by Theodore Ts'o.  Redistribution of this file is
 * permitted under the GNU General Public License.
 *
 * DES encryption plus some minor changes by Werner Almesberger, 30-MAY-1993
 * more DES encryption plus IDEA encryption by Nicholas J. Leon, June 20, 1996
 *
 * Modularized and updated for 1.1.16 kernel - Mitch Dsouza 28th May 1994
 * Adapted for 1.3.59 kernel - Andries Brouwer, 1 Feb 1996
 *
 * Fixed do_loop_request() re-entrancy - Vincent.Renardias@waw.com Mar 20, 1997
 *
 * Added devfs support - Richard Gooch <rgooch@atnf.csiro.au> 16-Jan-1998
 *
 * Handle sparse backing files correctly - Kenn Humborg, Jun 28, 1998
 *
 * Loadable modules and other fixes by AK, 1998
 *
 * Make real block number available to downstream transfer functions, enables
 * CBC (and relatives) mode encryption requiring unique IVs per data block.
 * Reed H. Petty, rhp@draper.net
 *
 * Maximum number of loop devices now dynamic via max_loop module parameter.
 * Russell Kroll <rkroll@exploits.org> 19990701
 *
 * Maximum number of loop devices when compiled-in now selectable by passing
 * max_loop=<1-255> to the kernel on boot.
 * Erik I. Bolsø, <eriki@himolde.no>, Oct 31, 1999
 *
 * Completely rewrite request handling to be make_request_fn style and
 * non blocking, pushing work to a helper thread. Lots of fixes from
 * Al Viro too.
 * Jens Axboe <axboe@suse.de>, Nov 2000
 *
 * Support up to 256 loop devices
 * Heinz Mauelshagen <mge@sistina.com>, Feb 2002
 *
 * Support for falling back on the write file operation when the address space
 * operations write_begin is not available on the backing filesystem.
 * Anton Altaparmakov, 16 Feb 2005
 *
 * Still To Fix:
 * - Advisory locking is ignored here.
 * - Should use an own CAP_* category instead of CAP_SYS_ADMIN
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/major.h>
#include <linux/wait.h>
#include <linux/blkdev.h>
#include <linux/blkpg.h>
#include <linux/init.h>
#include <linux/swap.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/suspend.h>
#include <linux/freezer.h>
#include <linux/mutex.h>
#include <linux/writeback.h>
#include <linux/completion.h>
#include <linux/highmem.h>
#include <linux/splice.h>
#include <linux/sysfs.h>
#include <linux/miscdevice.h>
#include <linux/falloc.h>
#include <linux/uio.h>
#include <linux/ioprio.h>
#include <linux/blk-cgroup.h>
#include <linux/sched/mm.h>
#include <linux/statfs.h>

#include "loop.h"

#include <linux/uaccess.h>

#define LOOP_IDLE_WORKER_TIMEOUT (60 * HZ)

static DEFINE_IDR(loop_index_idr);
static DEFINE_MUTEX(loop_ctl_mutex);
static DEFINE_MUTEX(loop_validate_mutex);

/**
 * loop_global_lock_killable() - take locks for safe loop_validate_file() test
 *
 * @lo: struct loop_device
 * @global: true if @lo is about to bind another "struct loop_device", false otherwise
 *
 * Returns 0 on success, -EINTR otherwise.
 *
 * Since loop_validate_file() traverses on other "struct loop_device" if
 * is_loop_device() is true, we need a global lock for serializing concurrent
 * loop_configure()/loop_change_fd()/__loop_clr_fd() calls.
 */
static int loop_global_lock_killable(struct loop_device *lo, bool global)
{
	int err;

	if (global) {
		err = mutex_lock_killable(&loop_validate_mutex);
		if (err)
			return err;
	}
	err = mutex_lock_killable(&lo->lo_mutex);
	if (err && global)
		mutex_unlock(&loop_validate_mutex);
	return err;
}

/**
 * loop_global_unlock() - release locks taken by loop_global_lock_killable()
 *
 * @lo: struct loop_device
 * @global: true if @lo was about to bind another "struct loop_device", false otherwise
 */
static void loop_global_unlock(struct loop_device *lo, bool global)
{
	mutex_unlock(&lo->lo_mutex);
	if (global)
		mutex_unlock(&loop_validate_mutex);
}

static int max_part;
static int part_shift;

static int transfer_xor(struct loop_device *lo, int cmd,
			struct page *raw_page, unsigned raw_off,
			struct page *loop_page, unsigned loop_off,
			int size, sector_t real_block)
{
	char *raw_buf = kmap_atomic(raw_page) + raw_off;
	char *loop_buf = kmap_atomic(loop_page) + loop_off;
	char *in, *out, *key;
	int i, keysize;

	if (cmd == READ) {
		in = raw_buf;
		out = loop_buf;
	} else {
		in = loop_buf;
		out = raw_buf;
	}

	key = lo->lo_encrypt_key;
	keysize = lo->lo_encrypt_key_size;
	for (i = 0; i < size; i++)
		*out++ = *in++ ^ key[(i & 511) % keysize];

	kunmap_atomic(loop_buf);
	kunmap_atomic(raw_buf);
	cond_resched();
	return 0;
}

static int xor_init(struct loop_device *lo, const struct loop_info64 *info)
{
	if (unlikely(info->lo_encrypt_key_size <= 0))
		return -EINVAL;
	return 0;
}

static struct loop_func_table none_funcs = {
	.number = LO_CRYPT_NONE,
}; 

static struct loop_func_table xor_funcs = {
	.number = LO_CRYPT_XOR,
	.transfer = transfer_xor,
	.init = xor_init
}; 

/* xfer_funcs[0] is special - its release function is never called */
static struct loop_func_table *xfer_funcs[MAX_LO_CRYPT] = {
	&none_funcs,
	&xor_funcs
};

static loff_t get_size(loff_t offset, loff_t sizelimit, struct file *file)
{
	loff_t loopsize;

	/* Compute loopsize in bytes */
	loopsize = i_size_read(file->f_mapping->host);
	if (offset > 0)
		loopsize -= offset;
	/* offset is beyond i_size, weird but possible */
	if (loopsize < 0)
		return 0;

	if (sizelimit > 0 && sizelimit < loopsize)
		loopsize = sizelimit;
	/*
	 * Unfortunately, if we want to do I/O on the device,
	 * the number of 512-byte sectors has to fit into a sector_t.
	 */
	return loopsize >> 9;
}

static loff_t get_loop_size(struct loop_device *lo, struct file *file)
{
	return get_size(lo->lo_offset, lo->lo_sizelimit, file);
}

static void __loop_update_dio(struct loop_device *lo, bool dio)
{
	struct file *file = lo->lo_backing_file;
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = mapping->host;
	unsigned short sb_bsize = 0;
	unsigned dio_align = 0;
	bool use_dio;

	if (inode->i_sb->s_bdev) {
		sb_bsize = bdev_logical_block_size(inode->i_sb->s_bdev);
		dio_align = sb_bsize - 1;
	}

	/*
	 * We support direct I/O only if lo_offset is aligned with the
	 * logical I/O size of backing device, and the logical block
	 * size of loop is bigger than the backing device's and the loop
	 * needn't transform transfer.
	 *
	 * TODO: the above condition may be loosed in the future, and
	 * direct I/O may be switched runtime at that time because most
	 * of requests in sane applications should be PAGE_SIZE aligned
	 */
	if (dio) {
		if (queue_logical_block_size(lo->lo_queue) >= sb_bsize &&
				!(lo->lo_offset & dio_align) &&
				mapping->a_ops->direct_IO &&
				!lo->transfer)
			use_dio = true;
		else
			use_dio = false;
	} else {
		use_dio = false;
	}

	if (lo->use_dio == use_dio)
		return;

	/* flush dirty pages before changing direct IO */
	vfs_fsync(file, 0);

	/*
	 * The flag of LO_FLAGS_DIRECT_IO is handled similarly with
	 * LO_FLAGS_READ_ONLY, both are set from kernel, and losetup
	 * will get updated by ioctl(LOOP_GET_STATUS)
	 */
	if (lo->lo_state == Lo_bound)
		blk_mq_freeze_queue(lo->lo_queue);
	lo->use_dio = use_dio;
	if (use_dio) {
		blk_queue_flag_clear(QUEUE_FLAG_NOMERGES, lo->lo_queue);
		lo->lo_flags |= LO_FLAGS_DIRECT_IO;
	} else {
		blk_queue_flag_set(QUEUE_FLAG_NOMERGES, lo->lo_queue);
		lo->lo_flags &= ~LO_FLAGS_DIRECT_IO;
	}
	if (lo->lo_state == Lo_bound)
		blk_mq_unfreeze_queue(lo->lo_queue);
}

/**
 * loop_set_size() - sets device size and notifies userspace
 * @lo: struct loop_device to set the size for
 * @size: new size of the loop device
 *
 * Callers must validate that the size passed into this function fits into
 * a sector_t, eg using loop_validate_size()
 */
static void loop_set_size(struct loop_device *lo, loff_t size)
{
	if (!set_capacity_and_notify(lo->lo_disk, size))
		kobject_uevent(&disk_to_dev(lo->lo_disk)->kobj, KOBJ_CHANGE);
}

static inline int
lo_do_transfer(struct loop_device *lo, int cmd,
	       struct page *rpage, unsigned roffs,
	       struct page *lpage, unsigned loffs,
	       int size, sector_t rblock)
{
	int ret;

	ret = lo->transfer(lo, cmd, rpage, roffs, lpage, loffs, size, rblock);
	if (likely(!ret))
		return 0;

	printk_ratelimited(KERN_ERR
		"loop: Transfer error at byte offset %llu, length %i.\n",
		(unsigned long long)rblock << 9, size);
	return ret;
}

static int lo_write_bvec(struct file *file, struct bio_vec *bvec, loff_t *ppos)
{
	struct iov_iter i;
	ssize_t bw;

	iov_iter_bvec(&i, WRITE, bvec, 1, bvec->bv_len);

	file_start_write(file);
	bw = vfs_iter_write(file, &i, ppos, 0);
	file_end_write(file);

	if (likely(bw ==  bvec->bv_len))
		return 0;

	printk_ratelimited(KERN_ERR
		"loop: Write error at byte offset %llu, length %i.\n",
		(unsigned long long)*ppos, bvec->bv_len);
	if (bw >= 0)
		bw = -EIO;
	return bw;
}

static int lo_write_simple(struct loop_device *lo, struct request *rq,
		loff_t pos)
{
	struct bio_vec bvec;
	struct req_iterator iter;
	int ret = 0;

	rq_for_each_segment(bvec, rq, iter) {
		ret = lo_write_bvec(lo->lo_backing_file, &bvec, &pos);
		if (ret < 0)
			break;
		cond_resched();
	}

	return ret;
}

/*
 * This is the slow, transforming version that needs to double buffer the
 * data as it cannot do the transformations in place without having direct
 * access to the destination pages of the backing file.
 */
static int lo_write_transfer(struct loop_device *lo, struct request *rq,
		loff_t pos)
{
	struct bio_vec bvec, b;
	struct req_iterator iter;
	struct page *page;
	int ret = 0;

	page = alloc_page(GFP_NOIO);
	if (unlikely(!page))
		return -ENOMEM;

	rq_for_each_segment(bvec, rq, iter) {
		ret = lo_do_transfer(lo, WRITE, page, 0, bvec.bv_page,
			bvec.bv_offset, bvec.bv_len, pos >> 9);
		if (unlikely(ret))
			break;

		b.bv_page = page;
		b.bv_offset = 0;
		b.bv_len = bvec.bv_len;
		ret = lo_write_bvec(lo->lo_backing_file, &b, &pos);
		if (ret < 0)
			break;
	}

	__free_page(page);
	return ret;
}

static int lo_read_simple(struct loop_device *lo, struct request *rq,
		loff_t pos)
{
	struct bio_vec bvec;
	struct req_iterator iter;
	struct iov_iter i;
	ssize_t len;

	rq_for_each_segment(bvec, rq, iter) {
		iov_iter_bvec(&i, READ, &bvec, 1, bvec.bv_len);
		len = vfs_iter_read(lo->lo_backing_file, &i, &pos, 0);
		if (len < 0)
			return len;

		flush_dcache_page(bvec.bv_page);

		if (len != bvec.bv_len) {
			struct bio *bio;

			__rq_for_each_bio(bio, rq)
				zero_fill_bio(bio);
			break;
		}
		cond_resched();
	}

	return 0;
}

static int lo_read_transfer(struct loop_device *lo, struct request *rq,
		loff_t pos)
{
	struct bio_vec bvec, b;
	struct req_iterator iter;
	struct iov_iter i;
	struct page *page;
	ssize_t len;
	int ret = 0;

	page = alloc_page(GFP_NOIO);
	if (unlikely(!page))
		return -ENOMEM;

	rq_for_each_segment(bvec, rq, iter) {
		loff_t offset = pos;

		b.bv_page = page;
		b.bv_offset = 0;
		b.bv_len = bvec.bv_len;

		iov_iter_bvec(&i, READ, &b, 1, b.bv_len);
		len = vfs_iter_read(lo->lo_backing_file, &i, &pos, 0);
		if (len < 0) {
			ret = len;
			goto out_free_page;
		}

		ret = lo_do_transfer(lo, READ, page, 0, bvec.bv_page,
			bvec.bv_offset, len, offset >> 9);
		if (ret)
			goto out_free_page;

		flush_dcache_page(bvec.bv_page);

		if (len != bvec.bv_len) {
			struct bio *bio;

			__rq_for_each_bio(bio, rq)
				zero_fill_bio(bio);
			break;
		}
	}

	ret = 0;
out_free_page:
	__free_page(page);
	return ret;
}

static int lo_fallocate(struct loop_device *lo, struct request *rq, loff_t pos,
			int mode)
{
	/*
	 * We use fallocate to manipulate the space mappings used by the image
	 * a.k.a. discard/zerorange. However we do not support this if
	 * encryption is enabled, because it may give an attacker useful
	 * information.
	 */
	struct file *file = lo->lo_backing_file;
	struct request_queue *q = lo->lo_queue;
	int ret;

	mode |= FALLOC_FL_KEEP_SIZE;

	if (!blk_queue_discard(q)) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	ret = file->f_op->fallocate(file, mode, pos, blk_rq_bytes(rq));
	if (unlikely(ret && ret != -EINVAL && ret != -EOPNOTSUPP))
		ret = -EIO;
 out:
	return ret;
}

static int lo_req_flush(struct loop_device *lo, struct request *rq)
{
	struct file *file = lo->lo_backing_file;
	int ret = vfs_fsync(file, 0);
	if (unlikely(ret && ret != -EINVAL))
		ret = -EIO;

	return ret;
}

static void lo_complete_rq(struct request *rq)
{
	struct loop_cmd *cmd = blk_mq_rq_to_pdu(rq);
	blk_status_t ret = BLK_STS_OK;

	if (!cmd->use_aio || cmd->ret < 0 || cmd->ret == blk_rq_bytes(rq) ||
	    req_op(rq) != REQ_OP_READ) {
		if (cmd->ret < 0)
			ret = errno_to_blk_status(cmd->ret);
		goto end_io;
	}

	/*
	 * Short READ - if we got some data, advance our request and
	 * retry it. If we got no data, end the rest with EIO.
	 */
	if (cmd->ret) {
		blk_update_request(rq, BLK_STS_OK, cmd->ret);
		cmd->ret = 0;
		blk_mq_requeue_request(rq, true);
	} else {
		if (cmd->use_aio) {
			struct bio *bio = rq->bio;

			while (bio) {
				zero_fill_bio(bio);
				bio = bio->bi_next;
			}
		}
		ret = BLK_STS_IOERR;
end_io:
		blk_mq_end_request(rq, ret);
	}
}

static void lo_rw_aio_do_completion(struct loop_cmd *cmd)
{
	struct request *rq = blk_mq_rq_from_pdu(cmd);

	if (!atomic_dec_and_test(&cmd->ref))
		return;
	kfree(cmd->bvec);
	cmd->bvec = NULL;
	if (likely(!blk_should_fake_timeout(rq->q)))
		blk_mq_complete_request(rq);
}

static void lo_rw_aio_complete(struct kiocb *iocb, long ret, long ret2)
{
	struct loop_cmd *cmd = container_of(iocb, struct loop_cmd, iocb);

	cmd->ret = ret;
	lo_rw_aio_do_completion(cmd);
}

static int lo_rw_aio(struct loop_device *lo, struct loop_cmd *cmd,
		     loff_t pos, bool rw)
{
	struct iov_iter iter;
	struct req_iterator rq_iter;
	struct bio_vec *bvec;
	struct request *rq = blk_mq_rq_from_pdu(cmd);
	struct bio *bio = rq->bio;
	struct file *file = lo->lo_backing_file;
	struct bio_vec tmp;
	unsigned int offset;
	int nr_bvec = 0;
	int ret;

	rq_for_each_bvec(tmp, rq, rq_iter)
		nr_bvec++;

	if (rq->bio != rq->biotail) {

		bvec = kmalloc_array(nr_bvec, sizeof(struct bio_vec),
				     GFP_NOIO);
		if (!bvec)
			return -EIO;
		cmd->bvec = bvec;

		/*
		 * The bios of the request may be started from the middle of
		 * the 'bvec' because of bio splitting, so we can't directly
		 * copy bio->bi_iov_vec to new bvec. The rq_for_each_bvec
		 * API will take care of all details for us.
		 */
		rq_for_each_bvec(tmp, rq, rq_iter) {
			*bvec = tmp;
			bvec++;
		}
		bvec = cmd->bvec;
		offset = 0;
	} else {
		/*
		 * Same here, this bio may be started from the middle of the
		 * 'bvec' because of bio splitting, so offset from the bvec
		 * must be passed to iov iterator
		 */
		offset = bio->bi_iter.bi_bvec_done;
		bvec = __bvec_iter_bvec(bio->bi_io_vec, bio->bi_iter);
	}
	atomic_set(&cmd->ref, 2);

	iov_iter_bvec(&iter, rw, bvec, nr_bvec, blk_rq_bytes(rq));
	iter.iov_offset = offset;

	cmd->iocb.ki_pos = pos;
	cmd->iocb.ki_filp = file;
	cmd->iocb.ki_complete = lo_rw_aio_complete;
	cmd->iocb.ki_flags = IOCB_DIRECT;
	cmd->iocb.ki_ioprio = req_get_ioprio(rq);

	if (rw == WRITE)
		ret = call_write_iter(file, &cmd->iocb, &iter);
	else
		ret = call_read_iter(file, &cmd->iocb, &iter);

	lo_rw_aio_do_completion(cmd);

	if (ret != -EIOCBQUEUED)
		cmd->iocb.ki_complete(&cmd->iocb, ret, 0);
	return 0;
}

static int do_req_filebacked(struct loop_device *lo, struct request *rq)
{
	struct loop_cmd *cmd = blk_mq_rq_to_pdu(rq);
	loff_t pos = ((loff_t) blk_rq_pos(rq) << 9) + lo->lo_offset;

	/*
	 * lo_write_simple and lo_read_simple should have been covered
	 * by io submit style function like lo_rw_aio(), one blocker
	 * is that lo_read_simple() need to call flush_dcache_page after
	 * the page is written from kernel, and it isn't easy to handle
	 * this in io submit style function which submits all segments
	 * of the req at one time. And direct read IO doesn't need to
	 * run flush_dcache_page().
	 */
	switch (req_op(rq)) {
	case REQ_OP_FLUSH:
		return lo_req_flush(lo, rq);
	case REQ_OP_WRITE_ZEROES:
		/*
		 * If the caller doesn't want deallocation, call zeroout to
		 * write zeroes the range.  Otherwise, punch them out.
		 */
		return lo_fallocate(lo, rq, pos,
			(rq->cmd_flags & REQ_NOUNMAP) ?
				FALLOC_FL_ZERO_RANGE :
				FALLOC_FL_PUNCH_HOLE);
	case REQ_OP_DISCARD:
		return lo_fallocate(lo, rq, pos, FALLOC_FL_PUNCH_HOLE);
	case REQ_OP_WRITE:
		if (lo->transfer)
			return lo_write_transfer(lo, rq, pos);
		else if (cmd->use_aio)
			return lo_rw_aio(lo, cmd, pos, WRITE);
		else
			return lo_write_simple(lo, rq, pos);
	case REQ_OP_READ:
		if (lo->transfer)
			return lo_read_transfer(lo, rq, pos);
		else if (cmd->use_aio)
			return lo_rw_aio(lo, cmd, pos, READ);
		else
			return lo_read_simple(lo, rq, pos);
	default:
		WARN_ON_ONCE(1);
		return -EIO;
	}
}

static inline void loop_update_dio(struct loop_device *lo)
{
	__loop_update_dio(lo, (lo->lo_backing_file->f_flags & O_DIRECT) |
				lo->use_dio);
}

static struct file *loop_real_file(struct file *file)
{
	struct file *f = NULL;

	if (file->f_path.dentry->d_sb->s_op->real_loop)
		f = file->f_path.dentry->d_sb->s_op->real_loop(file);
	return f;
}

static void loop_reread_partitions(struct loop_device *lo)
{
	int rc;

	mutex_lock(&lo->lo_disk->open_mutex);
	rc = bdev_disk_changed(lo->lo_disk, false);
	mutex_unlock(&lo->lo_disk->open_mutex);
	if (rc)
		pr_warn("%s: partition scan of loop%d (%s) failed (rc=%d)\n",
			__func__, lo->lo_number, lo->lo_file_name, rc);
}

static inline int is_loop_device(struct file *file)
{
	struct inode *i = file->f_mapping->host;

	return i && S_ISBLK(i->i_mode) && imajor(i) == LOOP_MAJOR;
}

static int loop_validate_file(struct file *file, struct block_device *bdev)
{
	struct inode	*inode = file->f_mapping->host;
	struct file	*f = file;

	/* Avoid recursion */
	while (is_loop_device(f)) {
		struct loop_device *l;

		lockdep_assert_held(&loop_validate_mutex);
		if (f->f_mapping->host->i_rdev == bdev->bd_dev)
			return -EBADF;

		l = I_BDEV(f->f_mapping->host)->bd_disk->private_data;
		if (l->lo_state != Lo_bound)
			return -EINVAL;
		/* Order wrt setting lo->lo_backing_file in loop_configure(). */
		rmb();
		f = l->lo_backing_file;
	}
	if (!S_ISREG(inode->i_mode) && !S_ISBLK(inode->i_mode))
		return -EINVAL;
	return 0;
}

/*
 * loop_change_fd switched the backing store of a loopback device to
 * a new file. This is useful for operating system installers to free up
 * the original file and in High Availability environments to switch to
 * an alternative location for the content in case of server meltdown.
 * This can only work if the loop device is used read-only, and if the
 * new backing store is the same size and type as the old backing store.
 */
static int loop_change_fd(struct loop_device *lo, struct block_device *bdev,
			  unsigned int arg)
{
	struct file *file = fget(arg);
	struct file *old_file;
	struct file *f, *virt_file = NULL, *old_virt_file;
	int error;
	bool partscan;
	bool is_loop;

	if (!file)
		return -EBADF;

	/* suppress uevents while reconfiguring the device */
	dev_set_uevent_suppress(disk_to_dev(lo->lo_disk), 1);

	is_loop = is_loop_device(file);
	error = loop_global_lock_killable(lo, is_loop);
	if (error)
		goto out_putf;
	error = -ENXIO;
	if (lo->lo_state != Lo_bound)
		goto out_err;

	/* the loop device has to be read-only */
	error = -EINVAL;
	if (!(lo->lo_flags & LO_FLAGS_READ_ONLY))
		goto out_err;

	f = loop_real_file(file);
	if (f) {
		virt_file = file;
		file = f;
		get_file(file);
	}

	error = loop_validate_file(file, bdev);
	if (error)
		goto out_err;

	old_file = lo->lo_backing_file;
	old_virt_file = lo->lo_backing_virt_file;

	error = -EINVAL;

	/* size of the new backing store needs to be the same */
	if (get_loop_size(lo, file) != get_loop_size(lo, old_file))
		goto out_err;

	/* and ... switch */
	disk_force_media_change(lo->lo_disk, DISK_EVENT_MEDIA_CHANGE);
	blk_mq_freeze_queue(lo->lo_queue);
	mapping_set_gfp_mask(old_file->f_mapping, lo->old_gfp_mask);
	lo->lo_backing_file = file;
	lo->lo_backing_virt_file = virt_file;
	lo->old_gfp_mask = mapping_gfp_mask(file->f_mapping);
	mapping_set_gfp_mask(file->f_mapping,
			     lo->old_gfp_mask & ~(__GFP_IO|__GFP_FS));
	loop_update_dio(lo);
	blk_mq_unfreeze_queue(lo->lo_queue);
	partscan = lo->lo_flags & LO_FLAGS_PARTSCAN;
	loop_global_unlock(lo, is_loop);

	/*
	 * Flush loop_validate_file() before fput(), for l->lo_backing_file
	 * might be pointing at old_file which might be the last reference.
	 */
	if (!is_loop) {
		mutex_lock(&loop_validate_mutex);
		mutex_unlock(&loop_validate_mutex);
	}
	/*
	 * We must drop file reference outside of lo_mutex as dropping
	 * the file ref can take open_mutex which creates circular locking
	 * dependency.
	 */
	fput(old_file);
	if (old_virt_file)
		fput(old_virt_file);
	dev_set_uevent_suppress(disk_to_dev(lo->lo_disk), 0);
	if (partscan)
		loop_reread_partitions(lo);

	error = 0;
done:
	kobject_uevent(&disk_to_dev(lo->lo_disk)->kobj, KOBJ_CHANGE);
	return error;

out_err:
	loop_global_unlock(lo, is_loop);
out_putf:
	fput(file);
	if (virt_file)
		fput(virt_file);
	dev_set_uevent_suppress(disk_to_dev(lo->lo_disk), 0);
	goto done;
}

/*
 * for AUFS
 * no get/put for file.
 */
struct file *loop_backing_file(struct super_block *sb)
{
	struct file *ret;
	struct loop_device *l;

	ret = NULL;
	if (MAJOR(sb->s_dev) == LOOP_MAJOR) {
		l = sb->s_bdev->bd_disk->private_data;
		ret = l->lo_backing_file;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(loop_backing_file);

/* loop sysfs attributes */

static ssize_t loop_attr_show(struct device *dev, char *page,
			      ssize_t (*callback)(struct loop_device *, char *))
{
	struct gendisk *disk = dev_to_disk(dev);
	struct loop_device *lo = disk->private_data;

	return callback(lo, page);
}

#define LOOP_ATTR_RO(_name)						\
static ssize_t loop_attr_##_name##_show(struct loop_device *, char *);	\
static ssize_t loop_attr_do_show_##_name(struct device *d,		\
				struct device_attribute *attr, char *b)	\
{									\
	return loop_attr_show(d, b, loop_attr_##_name##_show);		\
}									\
static struct device_attribute loop_attr_##_name =			\
	__ATTR(_name, 0444, loop_attr_do_show_##_name, NULL);

static ssize_t loop_attr_backing_file_show(struct loop_device *lo, char *buf)
{
	ssize_t ret;
	char *p = NULL;

	spin_lock_irq(&lo->lo_lock);
	if (lo->lo_backing_file)
		p = file_path(lo->lo_backing_file, buf, PAGE_SIZE - 1);
	spin_unlock_irq(&lo->lo_lock);

	if (IS_ERR_OR_NULL(p))
		ret = PTR_ERR(p);
	else {
		ret = strlen(p);
		memmove(buf, p, ret);
		buf[ret++] = '\n';
		buf[ret] = 0;
	}

	return ret;
}

static ssize_t loop_attr_offset_show(struct loop_device *lo, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (unsigned long long)lo->lo_offset);
}

static ssize_t loop_attr_sizelimit_show(struct loop_device *lo, char *buf)
{
	return sysfs_emit(buf, "%llu\n", (unsigned long long)lo->lo_sizelimit);
}

static ssize_t loop_attr_autoclear_show(struct loop_device *lo, char *buf)
{
	int autoclear = (lo->lo_flags & LO_FLAGS_AUTOCLEAR);

	return sysfs_emit(buf, "%s\n", autoclear ? "1" : "0");
}

static ssize_t loop_attr_partscan_show(struct loop_device *lo, char *buf)
{
	int partscan = (lo->lo_flags & LO_FLAGS_PARTSCAN);

	return sysfs_emit(buf, "%s\n", partscan ? "1" : "0");
}

static ssize_t loop_attr_dio_show(struct loop_device *lo, char *buf)
{
	int dio = (lo->lo_flags & LO_FLAGS_DIRECT_IO);

	return sysfs_emit(buf, "%s\n", dio ? "1" : "0");
}

LOOP_ATTR_RO(backing_file);
LOOP_ATTR_RO(offset);
LOOP_ATTR_RO(sizelimit);
LOOP_ATTR_RO(autoclear);
LOOP_ATTR_RO(partscan);
LOOP_ATTR_RO(dio);

static struct attribute *loop_attrs[] = {
	&loop_attr_backing_file.attr,
	&loop_attr_offset.attr,
	&loop_attr_sizelimit.attr,
	&loop_attr_autoclear.attr,
	&loop_attr_partscan.attr,
	&loop_attr_dio.attr,
	NULL,
};

static struct attribute_group loop_attribute_group = {
	.name = "loop",
	.attrs= loop_attrs,
};

static void loop_sysfs_init(struct loop_device *lo)
{
	lo->sysfs_inited = !sysfs_create_group(&disk_to_dev(lo->lo_disk)->kobj,
						&loop_attribute_group);
}

static void loop_sysfs_exit(struct loop_device *lo)
{
	if (lo->sysfs_inited)
		sysfs_remove_group(&disk_to_dev(lo->lo_disk)->kobj,
				   &loop_attribute_group);
}

static void loop_config_discard(struct loop_device *lo)
{
	struct file *file = lo->lo_backing_file;
	struct inode *inode = file->f_mapping->host;
	struct request_queue *q = lo->lo_queue;
	u32 granularity, max_discard_sectors;

	/*
	 * If the backing device is a block device, mirror its zeroing
	 * capability. Set the discard sectors to the block device's zeroing
	 * capabilities because loop discards result in blkdev_issue_zeroout(),
	 * not blkdev_issue_discard(). This maintains consistent behavior with
	 * file-backed loop devices: discarded regions read back as zero.
	 */
	if (S_ISBLK(inode->i_mode) && !lo->lo_encrypt_key_size) {
		struct request_queue *backingq = bdev_get_queue(I_BDEV(inode));

		max_discard_sectors = backingq->limits.max_write_zeroes_sectors;
		granularity = backingq->limits.discard_granularity ?:
			queue_physical_block_size(backingq);

	/*
	 * We use punch hole to reclaim the free space used by the
	 * image a.k.a. discard. However we do not support discard if
	 * encryption is enabled, because it may give an attacker
	 * useful information.
	 */
	} else if (!file->f_op->fallocate || lo->lo_encrypt_key_size) {
		max_discard_sectors = 0;
		granularity = 0;

	} else {
		struct kstatfs sbuf;

		max_discard_sectors = UINT_MAX >> 9;
		if (!vfs_statfs(&file->f_path, &sbuf))
			granularity = sbuf.f_bsize;
		else
			max_discard_sectors = 0;
	}

	if (max_discard_sectors) {
		q->limits.discard_granularity = granularity;
		blk_queue_max_discard_sectors(q, max_discard_sectors);
		blk_queue_max_write_zeroes_sectors(q, max_discard_sectors);
		blk_queue_flag_set(QUEUE_FLAG_DISCARD, q);
	} else {
		q->limits.discard_granularity = 0;
		blk_queue_max_discard_sectors(q, 0);
		blk_queue_max_write_zeroes_sectors(q, 0);
		blk_queue_flag_clear(QUEUE_FLAG_DISCARD, q);
	}
	q->limits.discard_alignment = 0;
}

struct loop_worker {
	struct rb_node rb_node;
	struct work_struct work;
	struct list_head cmd_list;
	struct list_head idle_list;
	struct loop_device *lo;
	struct cgroup_subsys_state *blkcg_css;
	unsigned long last_ran_at;
};

static void loop_workfn(struct work_struct *work);
static void loop_rootcg_workfn(struct work_struct *work);
static void loop_free_idle_workers(struct timer_list *timer);

#ifdef CONFIG_BLK_CGROUP
static inline int queue_on_root_worker(struct cgroup_subsys_state *css)
{
	return !css || css == blkcg_root_css;
}
#else
static inline int queue_on_root_worker(struct cgroup_subsys_state *css)
{
	return !css;
}
#endif

static void loop_queue_work(struct loop_device *lo, struct loop_cmd *cmd)
{
	struct rb_node **node = &(lo->worker_tree.rb_node), *parent = NULL;
	struct loop_worker *cur_worker, *worker = NULL;
	struct work_struct *work;
	struct list_head *cmd_list;

	spin_lock_irq(&lo->lo_work_lock);

	if (queue_on_root_worker(cmd->blkcg_css))
		goto queue_work;

	node = &lo->worker_tree.rb_node;

	while (*node) {
		parent = *node;
		cur_worker = container_of(*node, struct loop_worker, rb_node);
		if (cur_worker->blkcg_css == cmd->blkcg_css) {
			worker = cur_worker;
			break;
		} else if ((long)cur_worker->blkcg_css < (long)cmd->blkcg_css) {
			node = &(*node)->rb_left;
		} else {
			node = &(*node)->rb_right;
		}
	}
	if (worker)
		goto queue_work;

	worker = kzalloc(sizeof(struct loop_worker), GFP_NOWAIT | __GFP_NOWARN);
	/*
	 * In the event we cannot allocate a worker, just queue on the
	 * rootcg worker and issue the I/O as the rootcg
	 */
	if (!worker) {
		cmd->blkcg_css = NULL;
		if (cmd->memcg_css)
			css_put(cmd->memcg_css);
		cmd->memcg_css = NULL;
		goto queue_work;
	}

	worker->blkcg_css = cmd->blkcg_css;
	css_get(worker->blkcg_css);
	INIT_WORK(&worker->work, loop_workfn);
	INIT_LIST_HEAD(&worker->cmd_list);
	INIT_LIST_HEAD(&worker->idle_list);
	worker->lo = lo;
	rb_link_node(&worker->rb_node, parent, node);
	rb_insert_color(&worker->rb_node, &lo->worker_tree);
queue_work:
	if (worker) {
		/*
		 * We need to remove from the idle list here while
		 * holding the lock so that the idle timer doesn't
		 * free the worker
		 */
		if (!list_empty(&worker->idle_list))
			list_del_init(&worker->idle_list);
		work = &worker->work;
		cmd_list = &worker->cmd_list;
	} else {
		work = &lo->rootcg_work;
		cmd_list = &lo->rootcg_cmd_list;
	}
	list_add_tail(&cmd->list_entry, cmd_list);
	queue_work(lo->workqueue, work);
	spin_unlock_irq(&lo->lo_work_lock);
}

static void loop_update_rotational(struct loop_device *lo)
{
	struct file *file = lo->lo_backing_file;
	struct inode *file_inode = file->f_mapping->host;
	struct block_device *file_bdev = file_inode->i_sb->s_bdev;
	struct request_queue *q = lo->lo_queue;
	bool nonrot = true;

	/* not all filesystems (e.g. tmpfs) have a sb->s_bdev */
	if (file_bdev)
		nonrot = blk_queue_nonrot(bdev_get_queue(file_bdev));

	if (nonrot)
		blk_queue_flag_set(QUEUE_FLAG_NONROT, q);
	else
		blk_queue_flag_clear(QUEUE_FLAG_NONROT, q);
}

static int
loop_release_xfer(struct loop_device *lo)
{
	int err = 0;
	struct loop_func_table *xfer = lo->lo_encryption;

	if (xfer) {
		if (xfer->release)
			err = xfer->release(lo);
		lo->transfer = NULL;
		lo->lo_encryption = NULL;
		module_put(xfer->owner);
	}
	return err;
}

static int
loop_init_xfer(struct loop_device *lo, struct loop_func_table *xfer,
	       const struct loop_info64 *i)
{
	int err = 0;

	if (xfer) {
		struct module *owner = xfer->owner;

		if (!try_module_get(owner))
			return -EINVAL;
		if (xfer->init)
			err = xfer->init(lo, i);
		if (err)
			module_put(owner);
		else
			lo->lo_encryption = xfer;
	}
	return err;
}

/**
 * loop_set_status_from_info - configure device from loop_info
 * @lo: struct loop_device to configure
 * @info: struct loop_info64 to configure the device with
 *
 * Configures the loop device parameters according to the passed
 * in loop_info64 configuration.
 */
static int
loop_set_status_from_info(struct loop_device *lo,
			  const struct loop_info64 *info)
{
	int err;
	struct loop_func_table *xfer;
	kuid_t uid = current_uid();

	if ((unsigned int) info->lo_encrypt_key_size > LO_KEY_SIZE)
		return -EINVAL;

	err = loop_release_xfer(lo);
	if (err)
		return err;

	if (info->lo_encrypt_type) {
		unsigned int type = info->lo_encrypt_type;

		if (type >= MAX_LO_CRYPT)
			return -EINVAL;
		xfer = xfer_funcs[type];
		if (xfer == NULL)
			return -EINVAL;
	} else
		xfer = NULL;

	err = loop_init_xfer(lo, xfer, info);
	if (err)
		return err;

	/* Avoid assigning overflow values */
	if (info->lo_offset > LLONG_MAX || info->lo_sizelimit > LLONG_MAX)
		return -EOVERFLOW;

	lo->lo_offset = info->lo_offset;
	lo->lo_sizelimit = info->lo_sizelimit;

	memcpy(lo->lo_file_name, info->lo_file_name, LO_NAME_SIZE);
	memcpy(lo->lo_crypt_name, info->lo_crypt_name, LO_NAME_SIZE);
	lo->lo_file_name[LO_NAME_SIZE-1] = 0;
	lo->lo_crypt_name[LO_NAME_SIZE-1] = 0;

	if (!xfer)
		xfer = &none_funcs;
	lo->transfer = xfer->transfer;
	lo->ioctl = xfer->ioctl;

	lo->lo_flags = info->lo_flags;

	lo->lo_encrypt_key_size = info->lo_encrypt_key_size;
	lo->lo_init[0] = info->lo_init[0];
	lo->lo_init[1] = info->lo_init[1];
	if (info->lo_encrypt_key_size) {
		memcpy(lo->lo_encrypt_key, info->lo_encrypt_key,
		       info->lo_encrypt_key_size);
		lo->lo_key_owner = uid;
	}

	return 0;
}

static int loop_configure(struct loop_device *lo, fmode_t mode,
			  struct block_device *bdev,
			  const struct loop_config *config)
{
	struct file *file = fget(config->fd);
	struct file *f, *virt_file = NULL;
	struct inode *inode;
	struct address_space *mapping;
	int error;
	loff_t size;
	bool partscan;
	unsigned short bsize;
	bool is_loop;

	if (!file)
		return -EBADF;
	is_loop = is_loop_device(file);

	/* This is safe, since we have a reference from open(). */
	__module_get(THIS_MODULE);

	f = loop_real_file(file);
	if (f) {
		virt_file = file;
		file = f;
		get_file(file);
	}

	/*
	 * If we don't hold exclusive handle for the device, upgrade to it
	 * here to avoid changing device under exclusive owner.
	 */
	if (!(mode & FMODE_EXCL)) {
		error = bd_prepare_to_claim(bdev, loop_configure);
		if (error)
			goto out_putf;
	}

	error = loop_global_lock_killable(lo, is_loop);
	if (error)
		goto out_bdev;

	error = -EBUSY;
	if (lo->lo_state != Lo_unbound)
		goto out_unlock;

	error = loop_validate_file(file, bdev);
	if (error)
		goto out_unlock;

	mapping = file->f_mapping;
	inode = mapping->host;

	if ((config->info.lo_flags & ~LOOP_CONFIGURE_SETTABLE_FLAGS) != 0) {
		error = -EINVAL;
		goto out_unlock;
	}

	if (config->block_size) {
		error = blk_validate_block_size(config->block_size);
		if (error)
			goto out_unlock;
	}

	error = loop_set_status_from_info(lo, &config->info);
	if (error)
		goto out_unlock;

	if (!(file->f_mode & FMODE_WRITE) || !(mode & FMODE_WRITE) ||
	    !file->f_op->write_iter)
		lo->lo_flags |= LO_FLAGS_READ_ONLY;

	lo->workqueue = alloc_workqueue("loop%d",
					WQ_UNBOUND | WQ_FREEZABLE,
					0,
					lo->lo_number);
	if (!lo->workqueue) {
		error = -ENOMEM;
		goto out_unlock;
	}

	/* suppress uevents while reconfiguring the device */
	dev_set_uevent_suppress(disk_to_dev(lo->lo_disk), 1);

	disk_force_media_change(lo->lo_disk, DISK_EVENT_MEDIA_CHANGE);
	set_disk_ro(lo->lo_disk, (lo->lo_flags & LO_FLAGS_READ_ONLY) != 0);

	INIT_WORK(&lo->rootcg_work, loop_rootcg_workfn);
	INIT_LIST_HEAD(&lo->rootcg_cmd_list);
	INIT_LIST_HEAD(&lo->idle_worker_list);
	lo->worker_tree = RB_ROOT;
	timer_setup(&lo->timer, loop_free_idle_workers,
		TIMER_DEFERRABLE);
	lo->use_dio = lo->lo_flags & LO_FLAGS_DIRECT_IO;
	lo->lo_device = bdev;
	lo->lo_backing_file = file;
	lo->lo_backing_virt_file = virt_file;
	lo->old_gfp_mask = mapping_gfp_mask(mapping);
	mapping_set_gfp_mask(mapping, lo->old_gfp_mask & ~(__GFP_IO|__GFP_FS));

	if (!(lo->lo_flags & LO_FLAGS_READ_ONLY) && file->f_op->fsync)
		blk_queue_write_cache(lo->lo_queue, true, false);

	if (config->block_size)
		bsize = config->block_size;
	else if ((lo->lo_backing_file->f_flags & O_DIRECT) && inode->i_sb->s_bdev)
		/* In case of direct I/O, match underlying block size */
		bsize = bdev_logical_block_size(inode->i_sb->s_bdev);
	else
		bsize = 512;

	blk_queue_logical_block_size(lo->lo_queue, bsize);
	blk_queue_physical_block_size(lo->lo_queue, bsize);
	blk_queue_io_min(lo->lo_queue, bsize);

	loop_config_discard(lo);
	loop_update_rotational(lo);
	loop_update_dio(lo);
	loop_sysfs_init(lo);

	size = get_loop_size(lo, file);
	loop_set_size(lo, size);

	/* Order wrt reading lo_state in loop_validate_file(). */
	wmb();

	lo->lo_state = Lo_bound;
	if (part_shift)
		lo->lo_flags |= LO_FLAGS_PARTSCAN;
	partscan = lo->lo_flags & LO_FLAGS_PARTSCAN;
	if (partscan)
		clear_bit(GD_SUPPRESS_PART_SCAN, &lo->lo_disk->state);

	dev_set_uevent_suppress(disk_to_dev(lo->lo_disk), 0);
	kobject_uevent(&disk_to_dev(lo->lo_disk)->kobj, KOBJ_CHANGE);

	loop_global_unlock(lo, is_loop);
	if (partscan)
		loop_reread_partitions(lo);

	if (!(mode & FMODE_EXCL))
		bd_abort_claiming(bdev, loop_configure);

	return 0;

out_unlock:
	loop_global_unlock(lo, is_loop);
out_bdev:
	if (!(mode & FMODE_EXCL))
		bd_abort_claiming(bdev, loop_configure);
out_putf:
	fput(file);
	if (virt_file)
		fput(virt_file);
	/* This is safe: open() is still holding a reference. */
	module_put(THIS_MODULE);
	return error;
}

static int __loop_clr_fd(struct loop_device *lo, bool release)
{
	struct file *filp = NULL;
	struct file *virt_filp = lo->lo_backing_virt_file;
	gfp_t gfp = lo->old_gfp_mask;
	struct block_device *bdev = lo->lo_device;
	int err = 0;
	bool partscan = false;
	int lo_number;
	struct loop_worker *pos, *worker;

	/*
	 * Flush loop_configure() and loop_change_fd(). It is acceptable for
	 * loop_validate_file() to succeed, for actual clear operation has not
	 * started yet.
	 */
	mutex_lock(&loop_validate_mutex);
	mutex_unlock(&loop_validate_mutex);
	/*
	 * loop_validate_file() now fails because l->lo_state != Lo_bound
	 * became visible.
	 */

	mutex_lock(&lo->lo_mutex);
	if (WARN_ON_ONCE(lo->lo_state != Lo_rundown)) {
		err = -ENXIO;
		goto out_unlock;
	}

	filp = lo->lo_backing_file;
	if (filp == NULL) {
		err = -EINVAL;
		goto out_unlock;
	}

	if (test_bit(QUEUE_FLAG_WC, &lo->lo_queue->queue_flags))
		blk_queue_write_cache(lo->lo_queue, false, false);

	/* freeze request queue during the transition */
	blk_mq_freeze_queue(lo->lo_queue);

	destroy_workqueue(lo->workqueue);
	spin_lock_irq(&lo->lo_work_lock);
	list_for_each_entry_safe(worker, pos, &lo->idle_worker_list,
				idle_list) {
		list_del(&worker->idle_list);
		rb_erase(&worker->rb_node, &lo->worker_tree);
		css_put(worker->blkcg_css);
		kfree(worker);
	}
	spin_unlock_irq(&lo->lo_work_lock);
	del_timer_sync(&lo->timer);

	spin_lock_irq(&lo->lo_lock);
	lo->lo_backing_file = NULL;
	lo->lo_backing_virt_file = NULL;
	spin_unlock_irq(&lo->lo_lock);

	loop_release_xfer(lo);
	lo->transfer = NULL;
	lo->ioctl = NULL;
	lo->lo_device = NULL;
	lo->lo_encryption = NULL;
	lo->lo_offset = 0;
	lo->lo_sizelimit = 0;
	lo->lo_encrypt_key_size = 0;
	memset(lo->lo_encrypt_key, 0, LO_KEY_SIZE);
	memset(lo->lo_crypt_name, 0, LO_NAME_SIZE);
	memset(lo->lo_file_name, 0, LO_NAME_SIZE);
	blk_queue_logical_block_size(lo->lo_queue, 512);
	blk_queue_physical_block_size(lo->lo_queue, 512);
	blk_queue_io_min(lo->lo_queue, 512);
	if (bdev) {
		invalidate_bdev(bdev);
		bdev->bd_inode->i_mapping->wb_err = 0;
	}
	set_capacity(lo->lo_disk, 0);
	loop_sysfs_exit(lo);
	if (bdev) {
		/* let user-space know about this change */
		kobject_uevent(&disk_to_dev(bdev->bd_disk)->kobj, KOBJ_CHANGE);
	}
	mapping_set_gfp_mask(filp->f_mapping, gfp);
	/* This is safe: open() is still holding a reference. */
	module_put(THIS_MODULE);
	blk_mq_unfreeze_queue(lo->lo_queue);

	partscan = lo->lo_flags & LO_FLAGS_PARTSCAN && bdev;
	lo_number = lo->lo_number;
	disk_force_media_change(lo->lo_disk, DISK_EVENT_MEDIA_CHANGE);
out_unlock:
	mutex_unlock(&lo->lo_mutex);
	if (partscan) {
		/*
		 * open_mutex has been held already in release path, so don't
		 * acquire it if this function is called in such case.
		 *
		 * If the reread partition isn't from release path, lo_refcnt
		 * must be at least one and it can only become zero when the
		 * current holder is released.
		 */
		if (!release)
			mutex_lock(&lo->lo_disk->open_mutex);
		err = bdev_disk_changed(lo->lo_disk, false);
		if (!release)
			mutex_unlock(&lo->lo_disk->open_mutex);
		if (err)
			pr_warn("%s: partition scan of loop%d failed (rc=%d)\n",
				__func__, lo_number, err);
		/* Device is gone, no point in returning error */
		err = 0;
	}

	/*
	 * lo->lo_state is set to Lo_unbound here after above partscan has
	 * finished.
	 *
	 * There cannot be anybody else entering __loop_clr_fd() as
	 * lo->lo_backing_file is already cleared and Lo_rundown state
	 * protects us from all the other places trying to change the 'lo'
	 * device.
	 */
	mutex_lock(&lo->lo_mutex);
	lo->lo_flags = 0;
	if (!part_shift)
		set_bit(GD_SUPPRESS_PART_SCAN, &lo->lo_disk->state);
	lo->lo_state = Lo_unbound;
	mutex_unlock(&lo->lo_mutex);

	/*
	 * Need not hold lo_mutex to fput backing file. Calling fput holding
	 * lo_mutex triggers a circular lock dependency possibility warning as
	 * fput can take open_mutex which is usually taken before lo_mutex.
	 */
	if (filp)
		fput(filp);
	if (virt_filp)
		fput(virt_filp);
	return err;
}

static int loop_clr_fd(struct loop_device *lo)
{
	int err;

	err = mutex_lock_killable(&lo->lo_mutex);
	if (err)
		return err;
	if (lo->lo_state != Lo_bound) {
		mutex_unlock(&lo->lo_mutex);
		return -ENXIO;
	}
	/*
	 * If we've explicitly asked to tear down the loop device,
	 * and it has an elevated reference count, set it for auto-teardown when
	 * the last reference goes away. This stops $!~#$@ udev from
	 * preventing teardown because it decided that it needs to run blkid on
	 * the loopback device whenever they appear. xfstests is notorious for
	 * failing tests because blkid via udev races with a losetup
	 * <dev>/do something like mkfs/losetup -d <dev> causing the losetup -d
	 * command to fail with EBUSY.
	 */
	if (atomic_read(&lo->lo_refcnt) > 1) {
		lo->lo_flags |= LO_FLAGS_AUTOCLEAR;
		mutex_unlock(&lo->lo_mutex);
		return 0;
	}
	lo->lo_state = Lo_rundown;
	mutex_unlock(&lo->lo_mutex);

	return __loop_clr_fd(lo, false);
}

static int
loop_set_status(struct loop_device *lo, const struct loop_info64 *info)
{
	int err;
	kuid_t uid = current_uid();
	int prev_lo_flags;
	bool partscan = false;
	bool size_changed = false;

	err = mutex_lock_killable(&lo->lo_mutex);
	if (err)
		return err;
	if (lo->lo_encrypt_key_size &&
	    !uid_eq(lo->lo_key_owner, uid) &&
	    !capable(CAP_SYS_ADMIN)) {
		err = -EPERM;
		goto out_unlock;
	}
	if (lo->lo_state != Lo_bound) {
		err = -ENXIO;
		goto out_unlock;
	}

	if (lo->lo_offset != info->lo_offset ||
	    lo->lo_sizelimit != info->lo_sizelimit) {
		size_changed = true;
		sync_blockdev(lo->lo_device);
		invalidate_bdev(lo->lo_device);
	}

	/* I/O need to be drained during transfer transition */
	blk_mq_freeze_queue(lo->lo_queue);

	if (size_changed && lo->lo_device->bd_inode->i_mapping->nrpages) {
		/* If any pages were dirtied after invalidate_bdev(), try again */
		err = -EAGAIN;
		pr_warn("%s: loop%d (%s) has still dirty pages (nrpages=%lu)\n",
			__func__, lo->lo_number, lo->lo_file_name,
			lo->lo_device->bd_inode->i_mapping->nrpages);
		goto out_unfreeze;
	}

	prev_lo_flags = lo->lo_flags;

	err = loop_set_status_from_info(lo, info);
	if (err)
		goto out_unfreeze;

	/* Mask out flags that can't be set using LOOP_SET_STATUS. */
	lo->lo_flags &= LOOP_SET_STATUS_SETTABLE_FLAGS;
	/* For those flags, use the previous values instead */
	lo->lo_flags |= prev_lo_flags & ~LOOP_SET_STATUS_SETTABLE_FLAGS;
	/* For flags that can't be cleared, use previous values too */
	lo->lo_flags |= prev_lo_flags & ~LOOP_SET_STATUS_CLEARABLE_FLAGS;

	if (size_changed) {
		loff_t new_size = get_size(lo->lo_offset, lo->lo_sizelimit,
					   lo->lo_backing_file);
		loop_set_size(lo, new_size);
	}

	loop_config_discard(lo);

	/* update dio if lo_offset or transfer is changed */
	__loop_update_dio(lo, lo->use_dio);

out_unfreeze:
	blk_mq_unfreeze_queue(lo->lo_queue);

	if (!err && (lo->lo_flags & LO_FLAGS_PARTSCAN) &&
	     !(prev_lo_flags & LO_FLAGS_PARTSCAN)) {
		clear_bit(GD_SUPPRESS_PART_SCAN, &lo->lo_disk->state);
		partscan = true;
	}
out_unlock:
	mutex_unlock(&lo->lo_mutex);
	if (partscan)
		loop_reread_partitions(lo);

	return err;
}

static int
loop_get_status(struct loop_device *lo, struct loop_info64 *info)
{
	struct path path;
	struct kstat stat;
	int ret;

	ret = mutex_lock_killable(&lo->lo_mutex);
	if (ret)
		return ret;
	if (lo->lo_state != Lo_bound) {
		mutex_unlock(&lo->lo_mutex);
		return -ENXIO;
	}

	memset(info, 0, sizeof(*info));
	info->lo_number = lo->lo_number;
	info->lo_offset = lo->lo_offset;
	info->lo_sizelimit = lo->lo_sizelimit;
	info->lo_flags = lo->lo_flags;
	memcpy(info->lo_file_name, lo->lo_file_name, LO_NAME_SIZE);
	memcpy(info->lo_crypt_name, lo->lo_crypt_name, LO_NAME_SIZE);
	info->lo_encrypt_type =
		lo->lo_encryption ? lo->lo_encryption->number : 0;
	if (lo->lo_encrypt_key_size && capable(CAP_SYS_ADMIN)) {
		info->lo_encrypt_key_size = lo->lo_encrypt_key_size;
		memcpy(info->lo_encrypt_key, lo->lo_encrypt_key,
		       lo->lo_encrypt_key_size);
	}

	/* Drop lo_mutex while we call into the filesystem. */
	path = lo->lo_backing_file->f_path;
	path_get(&path);
	mutex_unlock(&lo->lo_mutex);
	ret = vfs_getattr(&path, &stat, STATX_INO, AT_STATX_SYNC_AS_STAT);
	if (!ret) {
		info->lo_device = huge_encode_dev(stat.dev);
		info->lo_inode = stat.ino;
		info->lo_rdevice = huge_encode_dev(stat.rdev);
	}
	path_put(&path);
	return ret;
}

static void
loop_info64_from_old(const struct loop_info *info, struct loop_info64 *info64)
{
	memset(info64, 0, sizeof(*info64));
	info64->lo_number = info->lo_number;
	info64->lo_device = info->lo_device;
	info64->lo_inode = info->lo_inode;
	info64->lo_rdevice = info->lo_rdevice;
	info64->lo_offset = info->lo_offset;
	info64->lo_sizelimit = 0;
	info64->lo_encrypt_type = info->lo_encrypt_type;
	info64->lo_encrypt_key_size = info->lo_encrypt_key_size;
	info64->lo_flags = info->lo_flags;
	info64->lo_init[0] = info->lo_init[0];
	info64->lo_init[1] = info->lo_init[1];
	if (info->lo_encrypt_type == LO_CRYPT_CRYPTOAPI)
		memcpy(info64->lo_crypt_name, info->lo_name, LO_NAME_SIZE);
	else
		memcpy(info64->lo_file_name, info->lo_name, LO_NAME_SIZE);
	memcpy(info64->lo_encrypt_key, info->lo_encrypt_key, LO_KEY_SIZE);
}

static int
loop_info64_to_old(const struct loop_info64 *info64, struct loop_info *info)
{
	memset(info, 0, sizeof(*info));
	info->lo_number = info64->lo_number;
	info->lo_device = info64->lo_device;
	info->lo_inode = info64->lo_inode;
	info->lo_rdevice = info64->lo_rdevice;
	info->lo_offset = info64->lo_offset;
	info->lo_encrypt_type = info64->lo_encrypt_type;
	info->lo_encrypt_key_size = info64->lo_encrypt_key_size;
	info->lo_flags = info64->lo_flags;
	info->lo_init[0] = info64->lo_init[0];
	info->lo_init[1] = info64->lo_init[1];
	if (info->lo_encrypt_type == LO_CRYPT_CRYPTOAPI)
		memcpy(info->lo_name, info64->lo_crypt_name, LO_NAME_SIZE);
	else
		memcpy(info->lo_name, info64->lo_file_name, LO_NAME_SIZE);
	memcpy(info->lo_encrypt_key, info64->lo_encrypt_key, LO_KEY_SIZE);

	/* error in case values were truncated */
	if (info->lo_device != info64->lo_device ||
	    info->lo_rdevice != info64->lo_rdevice ||
	    info->lo_inode != info64->lo_inode ||
	    info->lo_offset != info64->lo_offset)
		return -EOVERFLOW;

	return 0;
}

static int
loop_set_status_old(struct loop_device *lo, const struct loop_info __user *arg)
{
	struct loop_info info;
	struct loop_info64 info64;

	if (copy_from_user(&info, arg, sizeof (struct loop_info)))
		return -EFAULT;
	loop_info64_from_old(&info, &info64);
	return loop_set_status(lo, &info64);
}

static int
loop_set_status64(struct loop_device *lo, const struct loop_info64 __user *arg)
{
	struct loop_info64 info64;

	if (copy_from_user(&info64, arg, sizeof (struct loop_info64)))
		return -EFAULT;
	return loop_set_status(lo, &info64);
}

static int
loop_get_status_old(struct loop_device *lo, struct loop_info __user *arg) {
	struct loop_info info;
	struct loop_info64 info64;
	int err;

	if (!arg)
		return -EINVAL;
	err = loop_get_status(lo, &info64);
	if (!err)
		err = loop_info64_to_old(&info64, &info);
	if (!err && copy_to_user(arg, &info, sizeof(info)))
		err = -EFAULT;

	return err;
}

static int
loop_get_status64(struct loop_device *lo, struct loop_info64 __user *arg) {
	struct loop_info64 info64;
	int err;

	if (!arg)
		return -EINVAL;
	err = loop_get_status(lo, &info64);
	if (!err && copy_to_user(arg, &info64, sizeof(info64)))
		err = -EFAULT;

	return err;
}

static int loop_set_capacity(struct loop_device *lo)
{
	loff_t size;

	if (unlikely(lo->lo_state != Lo_bound))
		return -ENXIO;

	size = get_loop_size(lo, lo->lo_backing_file);
	loop_set_size(lo, size);

	return 0;
}

static int loop_set_dio(struct loop_device *lo, unsigned long arg)
{
	int error = -ENXIO;
	if (lo->lo_state != Lo_bound)
		goto out;

	__loop_update_dio(lo, !!arg);
	if (lo->use_dio == !!arg)
		return 0;
	error = -EINVAL;
 out:
	return error;
}

static int loop_set_block_size(struct loop_device *lo, unsigned long arg)
{
	int err = 0;

	if (lo->lo_state != Lo_bound)
		return -ENXIO;

	err = blk_validate_block_size(arg);
	if (err)
		return err;

	if (lo->lo_queue->limits.logical_block_size == arg)
		return 0;

	sync_blockdev(lo->lo_device);
	invalidate_bdev(lo->lo_device);

	blk_mq_freeze_queue(lo->lo_queue);

	/* invalidate_bdev should have truncated all the pages */
	if (lo->lo_device->bd_inode->i_mapping->nrpages) {
		err = -EAGAIN;
		pr_warn("%s: loop%d (%s) has still dirty pages (nrpages=%lu)\n",
			__func__, lo->lo_number, lo->lo_file_name,
			lo->lo_device->bd_inode->i_mapping->nrpages);
		goto out_unfreeze;
	}

	blk_queue_logical_block_size(lo->lo_queue, arg);
	blk_queue_physical_block_size(lo->lo_queue, arg);
	blk_queue_io_min(lo->lo_queue, arg);
	loop_update_dio(lo);
out_unfreeze:
	blk_mq_unfreeze_queue(lo->lo_queue);

	return err;
}

static int lo_simple_ioctl(struct loop_device *lo, unsigned int cmd,
			   unsigned long arg)
{
	int err;

	err = mutex_lock_killable(&lo->lo_mutex);
	if (err)
		return err;
	switch (cmd) {
	case LOOP_SET_CAPACITY:
		err = loop_set_capacity(lo);
		break;
	case LOOP_SET_DIRECT_IO:
		err = loop_set_dio(lo, arg);
		break;
	case LOOP_SET_BLOCK_SIZE:
		err = loop_set_block_size(lo, arg);
		break;
	default:
		err = lo->ioctl ? lo->ioctl(lo, cmd, arg) : -EINVAL;
	}
	mutex_unlock(&lo->lo_mutex);
	return err;
}

static int lo_ioctl(struct block_device *bdev, fmode_t mode,
	unsigned int cmd, unsigned long arg)
{
	struct loop_device *lo = bdev->bd_disk->private_data;
	void __user *argp = (void __user *) arg;
	int err;

	switch (cmd) {
	case LOOP_SET_FD: {
		/*
		 * Legacy case - pass in a zeroed out struct loop_config with
		 * only the file descriptor set , which corresponds with the
		 * default parameters we'd have used otherwise.
		 */
		struct loop_config config;

		memset(&config, 0, sizeof(config));
		config.fd = arg;

		return loop_configure(lo, mode, bdev, &config);
	}
	case LOOP_CONFIGURE: {
		struct loop_config config;

		if (copy_from_user(&config, argp, sizeof(config)))
			return -EFAULT;

		return loop_configure(lo, mode, bdev, &config);
	}
	case LOOP_CHANGE_FD:
		return loop_change_fd(lo, bdev, arg);
	case LOOP_CLR_FD:
		return loop_clr_fd(lo);
	case LOOP_SET_STATUS:
		err = -EPERM;
		if ((mode & FMODE_WRITE) || capable(CAP_SYS_ADMIN)) {
			err = loop_set_status_old(lo, argp);
		}
		break;
	case LOOP_GET_STATUS:
		return loop_get_status_old(lo, argp);
	case LOOP_SET_STATUS64:
		err = -EPERM;
		if ((mode & FMODE_WRITE) || capable(CAP_SYS_ADMIN)) {
			err = loop_set_status64(lo, argp);
		}
		break;
	case LOOP_GET_STATUS64:
		return loop_get_status64(lo, argp);
	case LOOP_SET_CAPACITY:
	case LOOP_SET_DIRECT_IO:
	case LOOP_SET_BLOCK_SIZE:
		if (!(mode & FMODE_WRITE) && !capable(CAP_SYS_ADMIN))
			return -EPERM;
		fallthrough;
	default:
		err = lo_simple_ioctl(lo, cmd, arg);
		break;
	}

	return err;
}

#ifdef CONFIG_COMPAT
struct compat_loop_info {
	compat_int_t	lo_number;      /* ioctl r/o */
	compat_dev_t	lo_device;      /* ioctl r/o */
	compat_ulong_t	lo_inode;       /* ioctl r/o */
	compat_dev_t	lo_rdevice;     /* ioctl r/o */
	compat_int_t	lo_offset;
	compat_int_t	lo_encrypt_type;
	compat_int_t	lo_encrypt_key_size;    /* ioctl w/o */
	compat_int_t	lo_flags;       /* ioctl r/o */
	char		lo_name[LO_NAME_SIZE];
	unsigned char	lo_encrypt_key[LO_KEY_SIZE]; /* ioctl w/o */
	compat_ulong_t	lo_init[2];
	char		reserved[4];
};

/*
 * Transfer 32-bit compatibility structure in userspace to 64-bit loop info
 * - noinlined to reduce stack space usage in main part of driver
 */
static noinline int
loop_info64_from_compat(const struct compat_loop_info __user *arg,
			struct loop_info64 *info64)
{
	struct compat_loop_info info;

	if (copy_from_user(&info, arg, sizeof(info)))
		return -EFAULT;

	memset(info64, 0, sizeof(*info64));
	info64->lo_number = info.lo_number;
	info64->lo_device = info.lo_device;
	info64->lo_inode = info.lo_inode;
	info64->lo_rdevice = info.lo_rdevice;
	info64->lo_offset = info.lo_offset;
	info64->lo_sizelimit = 0;
	info64->lo_encrypt_type = info.lo_encrypt_type;
	info64->lo_encrypt_key_size = info.lo_encrypt_key_size;
	info64->lo_flags = info.lo_flags;
	info64->lo_init[0] = info.lo_init[0];
	info64->lo_init[1] = info.lo_init[1];
	if (info.lo_encrypt_type == LO_CRYPT_CRYPTOAPI)
		memcpy(info64->lo_crypt_name, info.lo_name, LO_NAME_SIZE);
	else
		memcpy(info64->lo_file_name, info.lo_name, LO_NAME_SIZE);
	memcpy(info64->lo_encrypt_key, info.lo_encrypt_key, LO_KEY_SIZE);
	return 0;
}

/*
 * Transfer 64-bit loop info to 32-bit compatibility structure in userspace
 * - noinlined to reduce stack space usage in main part of driver
 */
static noinline int
loop_info64_to_compat(const struct loop_info64 *info64,
		      struct compat_loop_info __user *arg)
{
	struct compat_loop_info info;

	memset(&info, 0, sizeof(info));
	info.lo_number = info64->lo_number;
	info.lo_device = info64->lo_device;
	info.lo_inode = info64->lo_inode;
	info.lo_rdevice = info64->lo_rdevice;
	info.lo_offset = info64->lo_offset;
	info.lo_encrypt_type = info64->lo_encrypt_type;
	info.lo_encrypt_key_size = info64->lo_encrypt_key_size;
	info.lo_flags = info64->lo_flags;
	info.lo_init[0] = info64->lo_init[0];
	info.lo_init[1] = info64->lo_init[1];
	if (info.lo_encrypt_type == LO_CRYPT_CRYPTOAPI)
		memcpy(info.lo_name, info64->lo_crypt_name, LO_NAME_SIZE);
	else
		memcpy(info.lo_name, info64->lo_file_name, LO_NAME_SIZE);
	memcpy(info.lo_encrypt_key, info64->lo_encrypt_key, LO_KEY_SIZE);

	/* error in case values were truncated */
	if (info.lo_device != info64->lo_device ||
	    info.lo_rdevice != info64->lo_rdevice ||
	    info.lo_inode != info64->lo_inode ||
	    info.lo_offset != info64->lo_offset ||
	    info.lo_init[0] != info64->lo_init[0] ||
	    info.lo_init[1] != info64->lo_init[1])
		return -EOVERFLOW;

	if (copy_to_user(arg, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

static int
loop_set_status_compat(struct loop_device *lo,
		       const struct compat_loop_info __user *arg)
{
	struct loop_info64 info64;
	int ret;

	ret = loop_info64_from_compat(arg, &info64);
	if (ret < 0)
		return ret;
	return loop_set_status(lo, &info64);
}

static int
loop_get_status_compat(struct loop_device *lo,
		       struct compat_loop_info __user *arg)
{
	struct loop_info64 info64;
	int err;

	if (!arg)
		return -EINVAL;
	err = loop_get_status(lo, &info64);
	if (!err)
		err = loop_info64_to_compat(&info64, arg);
	return err;
}

static int lo_compat_ioctl(struct block_device *bdev, fmode_t mode,
			   unsigned int cmd, unsigned long arg)
{
	struct loop_device *lo = bdev->bd_disk->private_data;
	int err;

	switch(cmd) {
	case LOOP_SET_STATUS:
		err = loop_set_status_compat(lo,
			     (const struct compat_loop_info __user *)arg);
		break;
	case LOOP_GET_STATUS:
		err = loop_get_status_compat(lo,
				     (struct compat_loop_info __user *)arg);
		break;
	case LOOP_SET_CAPACITY:
	case LOOP_CLR_FD:
	case LOOP_GET_STATUS64:
	case LOOP_SET_STATUS64:
	case LOOP_CONFIGURE:
		arg = (unsigned long) compat_ptr(arg);
		fallthrough;
	case LOOP_SET_FD:
	case LOOP_CHANGE_FD:
	case LOOP_SET_BLOCK_SIZE:
	case LOOP_SET_DIRECT_IO:
		err = lo_ioctl(bdev, mode, cmd, arg);
		break;
	default:
		err = -ENOIOCTLCMD;
		break;
	}
	return err;
}
#endif

static int lo_open(struct block_device *bdev, fmode_t mode)
{
	struct loop_device *lo = bdev->bd_disk->private_data;
	int err;

	err = mutex_lock_killable(&lo->lo_mutex);
	if (err)
		return err;
	if (lo->lo_state == Lo_deleting)
		err = -ENXIO;
	else
		atomic_inc(&lo->lo_refcnt);
	mutex_unlock(&lo->lo_mutex);
	return err;
}

static void lo_release(struct gendisk *disk, fmode_t mode)
{
	struct loop_device *lo = disk->private_data;

	mutex_lock(&lo->lo_mutex);
	if (atomic_dec_return(&lo->lo_refcnt))
		goto out_unlock;

	if (lo->lo_flags & LO_FLAGS_AUTOCLEAR) {
		if (lo->lo_state != Lo_bound)
			goto out_unlock;
		lo->lo_state = Lo_rundown;
		mutex_unlock(&lo->lo_mutex);
		/*
		 * In autoclear mode, stop the loop thread
		 * and remove configuration after last close.
		 */
		__loop_clr_fd(lo, true);
		return;
	} else if (lo->lo_state == Lo_bound) {
		/*
		 * Otherwise keep thread (if running) and config,
		 * but flush possible ongoing bios in thread.
		 */
		blk_mq_freeze_queue(lo->lo_queue);
		blk_mq_unfreeze_queue(lo->lo_queue);
	}

out_unlock:
	mutex_unlock(&lo->lo_mutex);
}

static const struct block_device_operations lo_fops = {
	.owner =	THIS_MODULE,
	.open =		lo_open,
	.release =	lo_release,
	.ioctl =	lo_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl =	lo_compat_ioctl,
#endif
};

/*
 * And now the modules code and kernel interface.
 */

/*
 * If max_loop is specified, create that many devices upfront.
 * This also becomes a hard limit. If max_loop is not specified,
 * the default isn't a hard limit (as before commit 85c50197716c
 * changed the default value from 0 for max_loop=0 reasons), just
 * create CONFIG_BLK_DEV_LOOP_MIN_COUNT loop devices at module
 * init time. Loop devices can be requested on-demand with the
 * /dev/loop-control interface, or be instantiated by accessing
 * a 'dead' device node.
 */
static int max_loop = CONFIG_BLK_DEV_LOOP_MIN_COUNT;

static bool max_loop_specified;

static int max_loop_param_set_int(const char *val,
				  const struct kernel_param *kp)
{
	int ret;

	ret = param_set_int(val, kp);
	if (ret < 0)
		return ret;

	max_loop_specified = true;
	return 0;
}

static const struct kernel_param_ops max_loop_param_ops = {
	.set = max_loop_param_set_int,
	.get = param_get_int,
};

module_param_cb(max_loop, &max_loop_param_ops, &max_loop, 0444);
MODULE_PARM_DESC(max_loop, "Maximum number of loop devices");
module_param(max_part, int, 0444);
MODULE_PARM_DESC(max_part, "Maximum number of partitions per loop device");
MODULE_LICENSE("GPL");
MODULE_ALIAS_BLOCKDEV_MAJOR(LOOP_MAJOR);

int loop_register_transfer(struct loop_func_table *funcs)
{
	unsigned int n = funcs->number;

	if (n >= MAX_LO_CRYPT || xfer_funcs[n])
		return -EINVAL;
	xfer_funcs[n] = funcs;
	return 0;
}

int loop_unregister_transfer(int number)
{
	unsigned int n = number;
	struct loop_func_table *xfer;

	if (n == 0 || n >= MAX_LO_CRYPT || (xfer = xfer_funcs[n]) == NULL)
		return -EINVAL;
	/*
	 * This function is called from only cleanup_cryptoloop().
	 * Given that each loop device that has a transfer enabled holds a
	 * reference to the module implementing it we should never get here
	 * with a transfer that is set (unless forced module unloading is
	 * requested). Thus, check module's refcount and warn if this is
	 * not a clean unloading.
	 */
#ifdef CONFIG_MODULE_UNLOAD
	if (xfer->owner && module_refcount(xfer->owner) != -1)
		pr_err("Danger! Unregistering an in use transfer function.\n");
#endif

	xfer_funcs[n] = NULL;
	return 0;
}

EXPORT_SYMBOL(loop_register_transfer);
EXPORT_SYMBOL(loop_unregister_transfer);

static blk_status_t loop_queue_rq(struct blk_mq_hw_ctx *hctx,
		const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct loop_cmd *cmd = blk_mq_rq_to_pdu(rq);
	struct loop_device *lo = rq->q->queuedata;

	blk_mq_start_request(rq);

	if (lo->lo_state != Lo_bound)
		return BLK_STS_IOERR;

	switch (req_op(rq)) {
	case REQ_OP_FLUSH:
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		cmd->use_aio = false;
		break;
	default:
		cmd->use_aio = lo->use_dio;
		break;
	}

	/* always use the first bio's css */
	cmd->blkcg_css = NULL;
	cmd->memcg_css = NULL;
#ifdef CONFIG_BLK_CGROUP
	if (rq->bio && rq->bio->bi_blkg) {
		cmd->blkcg_css = &bio_blkcg(rq->bio)->css;
#ifdef CONFIG_MEMCG
		cmd->memcg_css =
			cgroup_get_e_css(cmd->blkcg_css->cgroup,
					&memory_cgrp_subsys);
#endif
	}
#endif
	loop_queue_work(lo, cmd);

	return BLK_STS_OK;
}

static void loop_handle_cmd(struct loop_cmd *cmd)
{
	struct cgroup_subsys_state *cmd_blkcg_css = cmd->blkcg_css;
	struct cgroup_subsys_state *cmd_memcg_css = cmd->memcg_css;
	struct request *rq = blk_mq_rq_from_pdu(cmd);
	const bool write = op_is_write(req_op(rq));
	struct loop_device *lo = rq->q->queuedata;
	int ret = 0;
	struct mem_cgroup *old_memcg = NULL;
	const bool use_aio = cmd->use_aio;

	if (write && (lo->lo_flags & LO_FLAGS_READ_ONLY)) {
		ret = -EIO;
		goto failed;
	}

	if (cmd_blkcg_css)
		kthread_associate_blkcg(cmd_blkcg_css);
	if (cmd_memcg_css)
		old_memcg = set_active_memcg(
			mem_cgroup_from_css(cmd_memcg_css));

	/*
	 * do_req_filebacked() may call blk_mq_complete_request() synchronously
	 * or asynchronously if using aio. Hence, do not touch 'cmd' after
	 * do_req_filebacked() has returned unless we are sure that 'cmd' has
	 * not yet been completed.
	 */
	ret = do_req_filebacked(lo, rq);

	if (cmd_blkcg_css)
		kthread_associate_blkcg(NULL);

	if (cmd_memcg_css) {
		set_active_memcg(old_memcg);
		css_put(cmd_memcg_css);
	}
 failed:
	/* complete non-aio request */
	if (!use_aio || ret) {
		if (ret == -EOPNOTSUPP)
			cmd->ret = ret;
		else
			cmd->ret = ret ? -EIO : 0;
		if (likely(!blk_should_fake_timeout(rq->q)))
			blk_mq_complete_request(rq);
	}
}

static void loop_set_timer(struct loop_device *lo)
{
	timer_reduce(&lo->timer, jiffies + LOOP_IDLE_WORKER_TIMEOUT);
}

static void loop_process_work(struct loop_worker *worker,
			struct list_head *cmd_list, struct loop_device *lo)
{
	int orig_flags = current->flags;
	struct loop_cmd *cmd;

	current->flags |= PF_LOCAL_THROTTLE | PF_MEMALLOC_NOIO;
	spin_lock_irq(&lo->lo_work_lock);
	while (!list_empty(cmd_list)) {
		cmd = container_of(
			cmd_list->next, struct loop_cmd, list_entry);
		list_del(cmd_list->next);
		spin_unlock_irq(&lo->lo_work_lock);

		loop_handle_cmd(cmd);
		cond_resched();

		spin_lock_irq(&lo->lo_work_lock);
	}

	/*
	 * We only add to the idle list if there are no pending cmds
	 * *and* the worker will not run again which ensures that it
	 * is safe to free any worker on the idle list
	 */
	if (worker && !work_pending(&worker->work)) {
		worker->last_ran_at = jiffies;
		list_add_tail(&worker->idle_list, &lo->idle_worker_list);
		loop_set_timer(lo);
	}
	spin_unlock_irq(&lo->lo_work_lock);
	current->flags = orig_flags;
}

static void loop_workfn(struct work_struct *work)
{
	struct loop_worker *worker =
		container_of(work, struct loop_worker, work);
	loop_process_work(worker, &worker->cmd_list, worker->lo);
}

static void loop_rootcg_workfn(struct work_struct *work)
{
	struct loop_device *lo =
		container_of(work, struct loop_device, rootcg_work);
	loop_process_work(NULL, &lo->rootcg_cmd_list, lo);
}

static void loop_free_idle_workers(struct timer_list *timer)
{
	struct loop_device *lo = container_of(timer, struct loop_device, timer);
	struct loop_worker *pos, *worker;

	spin_lock_irq(&lo->lo_work_lock);
	list_for_each_entry_safe(worker, pos, &lo->idle_worker_list,
				idle_list) {
		if (time_is_after_jiffies(worker->last_ran_at +
						LOOP_IDLE_WORKER_TIMEOUT))
			break;
		list_del(&worker->idle_list);
		rb_erase(&worker->rb_node, &lo->worker_tree);
		css_put(worker->blkcg_css);
		kfree(worker);
	}
	if (!list_empty(&lo->idle_worker_list))
		loop_set_timer(lo);
	spin_unlock_irq(&lo->lo_work_lock);
}

static const struct blk_mq_ops loop_mq_ops = {
	.queue_rq       = loop_queue_rq,
	.complete	= lo_complete_rq,
};

static int loop_add(int i)
{
	struct loop_device *lo;
	struct gendisk *disk;
	int err;

	err = -ENOMEM;
	lo = kzalloc(sizeof(*lo), GFP_KERNEL);
	if (!lo)
		goto out;
	lo->lo_state = Lo_unbound;

	err = mutex_lock_killable(&loop_ctl_mutex);
	if (err)
		goto out_free_dev;

	/* allocate id, if @id >= 0, we're requesting that specific id */
	if (i >= 0) {
		err = idr_alloc(&loop_index_idr, lo, i, i + 1, GFP_KERNEL);
		if (err == -ENOSPC)
			err = -EEXIST;
	} else {
		err = idr_alloc(&loop_index_idr, lo, 0, 0, GFP_KERNEL);
	}
	mutex_unlock(&loop_ctl_mutex);
	if (err < 0)
		goto out_free_dev;
	i = err;

	err = -ENOMEM;
	lo->tag_set.ops = &loop_mq_ops;
	lo->tag_set.nr_hw_queues = 1;
	lo->tag_set.queue_depth = 128;
	lo->tag_set.numa_node = NUMA_NO_NODE;
	lo->tag_set.cmd_size = sizeof(struct loop_cmd);
	lo->tag_set.flags = BLK_MQ_F_SHOULD_MERGE | BLK_MQ_F_STACKING |
		BLK_MQ_F_NO_SCHED_BY_DEFAULT;
	lo->tag_set.driver_data = lo;

	err = blk_mq_alloc_tag_set(&lo->tag_set);
	if (err)
		goto out_free_idr;

	disk = lo->lo_disk = blk_mq_alloc_disk(&lo->tag_set, lo);
	if (IS_ERR(disk)) {
		err = PTR_ERR(disk);
		goto out_cleanup_tags;
	}
	lo->lo_queue = lo->lo_disk->queue;

	blk_queue_max_hw_sectors(lo->lo_queue, BLK_DEF_MAX_SECTORS);

	/*
	 * By default, we do buffer IO, so it doesn't make sense to enable
	 * merge because the I/O submitted to backing file is handled page by
	 * page. For directio mode, merge does help to dispatch bigger request
	 * to underlayer disk. We will enable merge once directio is enabled.
	 */
	blk_queue_flag_set(QUEUE_FLAG_NOMERGES, lo->lo_queue);

	/*
	 * Disable partition scanning by default. The in-kernel partition
	 * scanning can be requested individually per-device during its
	 * setup. Userspace can always add and remove partitions from all
	 * devices. The needed partition minors are allocated from the
	 * extended minor space, the main loop device numbers will continue
	 * to match the loop minors, regardless of the number of partitions
	 * used.
	 *
	 * If max_part is given, partition scanning is globally enabled for
	 * all loop devices. The minors for the main loop devices will be
	 * multiples of max_part.
	 *
	 * Note: Global-for-all-devices, set-only-at-init, read-only module
	 * parameteters like 'max_loop' and 'max_part' make things needlessly
	 * complicated, are too static, inflexible and may surprise
	 * userspace tools. Parameters like this in general should be avoided.
	 */
	if (!part_shift)
		set_bit(GD_SUPPRESS_PART_SCAN, &disk->state);
	disk->flags |= GENHD_FL_EXT_DEVT;
	atomic_set(&lo->lo_refcnt, 0);
	mutex_init(&lo->lo_mutex);
	lo->lo_number		= i;
	spin_lock_init(&lo->lo_lock);
	spin_lock_init(&lo->lo_work_lock);
	disk->major		= LOOP_MAJOR;
	disk->first_minor	= i << part_shift;
	disk->minors		= 1 << part_shift;
	disk->fops		= &lo_fops;
	disk->private_data	= lo;
	disk->queue		= lo->lo_queue;
	disk->events		= DISK_EVENT_MEDIA_CHANGE;
	disk->event_flags	= DISK_EVENT_FLAG_UEVENT;
	sprintf(disk->disk_name, "loop%d", i);
	/* Make this loop device reachable from pathname. */
	add_disk(disk);
	/* Show this loop device. */
	mutex_lock(&loop_ctl_mutex);
	lo->idr_visible = true;
	mutex_unlock(&loop_ctl_mutex);
	return i;

out_cleanup_tags:
	blk_mq_free_tag_set(&lo->tag_set);
out_free_idr:
	mutex_lock(&loop_ctl_mutex);
	idr_remove(&loop_index_idr, i);
	mutex_unlock(&loop_ctl_mutex);
out_free_dev:
	kfree(lo);
out:
	return err;
}

static void loop_remove(struct loop_device *lo)
{
	/* Make this loop device unreachable from pathname. */
	del_gendisk(lo->lo_disk);
	blk_cleanup_disk(lo->lo_disk);
	blk_mq_free_tag_set(&lo->tag_set);
	mutex_lock(&loop_ctl_mutex);
	idr_remove(&loop_index_idr, lo->lo_number);
	mutex_unlock(&loop_ctl_mutex);
	/* There is no route which can find this loop device. */
	mutex_destroy(&lo->lo_mutex);
	kfree(lo);
}

static void loop_probe(dev_t dev)
{
	int idx = MINOR(dev) >> part_shift;

	if (max_loop_specified && max_loop && idx >= max_loop)
		return;
	loop_add(idx);
}

static int loop_control_remove(int idx)
{
	struct loop_device *lo;
	int ret;

	if (idx < 0) {
		pr_warn_once("deleting an unspecified loop device is not supported.\n");
		return -EINVAL;
	}
		
	/* Hide this loop device for serialization. */
	ret = mutex_lock_killable(&loop_ctl_mutex);
	if (ret)
		return ret;
	lo = idr_find(&loop_index_idr, idx);
	if (!lo || !lo->idr_visible)
		ret = -ENODEV;
	else
		lo->idr_visible = false;
	mutex_unlock(&loop_ctl_mutex);
	if (ret)
		return ret;

	/* Check whether this loop device can be removed. */
	ret = mutex_lock_killable(&lo->lo_mutex);
	if (ret)
		goto mark_visible;
	if (lo->lo_state != Lo_unbound ||
	    atomic_read(&lo->lo_refcnt) > 0) {
		mutex_unlock(&lo->lo_mutex);
		ret = -EBUSY;
		goto mark_visible;
	}
	/* Mark this loop device no longer open()-able. */
	lo->lo_state = Lo_deleting;
	mutex_unlock(&lo->lo_mutex);

	loop_remove(lo);
	return 0;

mark_visible:
	/* Show this loop device again. */
	mutex_lock(&loop_ctl_mutex);
	lo->idr_visible = true;
	mutex_unlock(&loop_ctl_mutex);
	return ret;
}

static int loop_control_get_free(int idx)
{
	struct loop_device *lo;
	int id, ret;

	ret = mutex_lock_killable(&loop_ctl_mutex);
	if (ret)
		return ret;
	idr_for_each_entry(&loop_index_idr, lo, id) {
		/* Hitting a race results in creating a new loop device which is harmless. */
		if (lo->idr_visible && data_race(lo->lo_state) == Lo_unbound)
			goto found;
	}
	mutex_unlock(&loop_ctl_mutex);
	return loop_add(-1);
found:
	mutex_unlock(&loop_ctl_mutex);
	return id;
}

static long loop_control_ioctl(struct file *file, unsigned int cmd,
			       unsigned long parm)
{
	switch (cmd) {
	case LOOP_CTL_ADD:
		return loop_add(parm);
	case LOOP_CTL_REMOVE:
		return loop_control_remove(parm);
	case LOOP_CTL_GET_FREE:
		return loop_control_get_free(parm);
	default:
		return -ENOSYS;
	}
}

static const struct file_operations loop_ctl_fops = {
	.open		= nonseekable_open,
	.unlocked_ioctl	= loop_control_ioctl,
	.compat_ioctl	= loop_control_ioctl,
	.owner		= THIS_MODULE,
	.llseek		= noop_llseek,
};

static struct miscdevice loop_misc = {
	.minor		= LOOP_CTRL_MINOR,
	.name		= "loop-control",
	.fops		= &loop_ctl_fops,
};

MODULE_ALIAS_MISCDEV(LOOP_CTRL_MINOR);
MODULE_ALIAS("devname:loop-control");

static int __init loop_init(void)
{
	int i;
	int err;

	part_shift = 0;
	if (max_part > 0) {
		part_shift = fls(max_part);

		/*
		 * Adjust max_part according to part_shift as it is exported
		 * to user space so that user can decide correct minor number
		 * if [s]he want to create more devices.
		 *
		 * Note that -1 is required because partition 0 is reserved
		 * for the whole disk.
		 */
		max_part = (1UL << part_shift) - 1;
	}

	if ((1UL << part_shift) > DISK_MAX_PARTS) {
		err = -EINVAL;
		goto err_out;
	}

	if (max_loop > 1UL << (MINORBITS - part_shift)) {
		err = -EINVAL;
		goto err_out;
	}

	err = misc_register(&loop_misc);
	if (err < 0)
		goto err_out;


	if (__register_blkdev(LOOP_MAJOR, "loop", loop_probe)) {
		err = -EIO;
		goto misc_out;
	}

	/* pre-create number of devices given by config or max_loop */
	for (i = 0; i < max_loop; i++)
		loop_add(i);

	printk(KERN_INFO "loop: module loaded\n");
	return 0;

misc_out:
	misc_deregister(&loop_misc);
err_out:
	return err;
}

static void __exit loop_exit(void)
{
	struct loop_device *lo;
	int id;

	unregister_blkdev(LOOP_MAJOR, "loop");
	misc_deregister(&loop_misc);

	/*
	 * There is no need to use loop_ctl_mutex here, for nobody else can
	 * access loop_index_idr when this module is unloading (unless forced
	 * module unloading is requested). If this is not a clean unloading,
	 * we have no means to avoid kernel crash.
	 */
	idr_for_each_entry(&loop_index_idr, lo, id)
		loop_remove(lo);

	idr_destroy(&loop_index_idr);
}

module_init(loop_init);
module_exit(loop_exit);

#ifndef MODULE
static int __init max_loop_setup(char *str)
{
	max_loop = simple_strtol(str, NULL, 0);
	max_loop_specified = true;
	return 1;
}

__setup("max_loop=", max_loop_setup);
#endif
