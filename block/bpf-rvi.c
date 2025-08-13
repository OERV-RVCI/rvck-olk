#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/seq_file.h>
#include <linux/part_stat.h>
#include <linux/pid_namespace.h>
#include <linux/bpf.h>

#include "blk-mq.h"
#include "blk-cgroup.h"

/*
 * Diskstats
 */

struct diskstats_seq_priv {
	struct class_dev_iter iter;	// must be the first,
					// to let us reuse disk_seqf_next()
	struct blkcg *task_blkcg;
};

/*
 * Basically the same with disk_seqf_start() but without allocating iter and
 * then overwriting seqf->private, which points to priv_data->target_private
 * in bpf_iter case (see prepare_seq_file()), and is needed to retrieve
 * struct bpf_iter_priv_data. Here we allocate iter via setting
 * .seq_priv_size and turning priv_data->target_private into iter.
 */
static void *bpf_disk_seqf_start(struct seq_file *seqf, loff_t *pos)
{
	loff_t skip = *pos;
	struct diskstats_seq_priv *priv = seqf->private;
	struct class_dev_iter *iter;
	struct device *dev;
	struct task_struct *reaper = get_current_level1_reaper();

	priv->task_blkcg = css_to_blkcg(task_css(reaper ?: current, io_cgrp_id));
	if (reaper)
		put_task_struct(reaper);

	iter = &priv->iter;
	class_dev_iter_init(iter, &block_class, NULL, &disk_type);
	do {
		dev = class_dev_iter_next(iter);
		if (!dev)
			return NULL;
	} while (skip--);

	return dev_to_disk(dev);
}

/*
 * Similar to the difference between {bpf_,}disk_seqf_start,
 * here we don't free iter.
 */
static void bpf_disk_seqf_stop(struct seq_file *seqf, void *v)
{
	struct diskstats_seq_priv *priv = seqf->private;
	struct class_dev_iter *iter = &priv->iter;

	/* stop is called even after start failed :-( */
	if (iter)
		class_dev_iter_exit(iter);
}

/* Same with disk_seqf_next() */
static void *bpf_disk_seqf_next(struct seq_file *seqf, void *v, loff_t *pos)
{
	struct device *dev;

	(*pos)++;
	dev = class_dev_iter_next(seqf->private);
	if (dev)
		return dev_to_disk(dev);

	return NULL;
}

struct bpf_iter__diskstats {
	__bpf_md_ptr(struct bpf_iter_meta *, meta);
	__bpf_md_ptr(struct block_device *, bd);
	__bpf_md_ptr(struct disk_stats *, native_stat);
	unsigned int inflight __aligned(8);
	__bpf_md_ptr(struct blkcg *, task_blkcg);
};

DEFINE_BPF_ITER_FUNC(diskstats, struct bpf_iter_meta *meta,
		     struct block_device *bd, struct disk_stats *native_stat,
		     uint inflight,
		     struct blkcg *task_blkcg)

static int native_diskstats_show(struct seq_file *seqf, struct block_device *hd,
				 struct disk_stats *stat, unsigned int inflight)
{
	seq_printf(seqf, "%4d %7d %pg "
		   "%lu %lu %lu %u "
		   "%lu %lu %lu %u "
		   "%u %u %u "
		   "%lu %lu %lu %u "
		   "%lu %u"
		   "\n",
		   MAJOR(hd->bd_dev), MINOR(hd->bd_dev), hd,
		   stat->ios[STAT_READ],
		   stat->merges[STAT_READ],
		   stat->sectors[STAT_READ],
		   (unsigned int)div_u64(stat->nsecs[STAT_READ],
						NSEC_PER_MSEC),
		   stat->ios[STAT_WRITE],
		   stat->merges[STAT_WRITE],
		   stat->sectors[STAT_WRITE],
		   (unsigned int)div_u64(stat->nsecs[STAT_WRITE],
						NSEC_PER_MSEC),
		   inflight,
		   jiffies_to_msecs(stat->io_ticks),
		   (unsigned int)div_u64(stat->nsecs[STAT_READ] +
					 stat->nsecs[STAT_WRITE] +
					 stat->nsecs[STAT_DISCARD] +
					 stat->nsecs[STAT_FLUSH],
						NSEC_PER_MSEC),
		   stat->ios[STAT_DISCARD],
		   stat->merges[STAT_DISCARD],
		   stat->sectors[STAT_DISCARD],
		   (unsigned int)div_u64(stat->nsecs[STAT_DISCARD],
					 NSEC_PER_MSEC),
		   stat->ios[STAT_FLUSH],
		   (unsigned int)div_u64(stat->nsecs[STAT_FLUSH],
					 NSEC_PER_MSEC)
		);
	return 0;
}

static int __diskstats_show(struct seq_file *seqf, struct block_device *hd,
			    struct disk_stats *stat, unsigned int inflight)
{
	struct bpf_iter__diskstats ctx;
	struct bpf_iter_meta meta;
	struct bpf_prog *prog;
	struct diskstats_seq_priv *priv = seqf->private;

	meta.seq = seqf;
	prog = bpf_iter_get_info(&meta, false);
	if (!prog)
		return native_diskstats_show(seqf, hd, stat, inflight);

	ctx.meta = &meta;
	ctx.bd = hd;
	ctx.native_stat = stat;
	ctx.inflight = inflight;
	ctx.task_blkcg = priv->task_blkcg;
	return bpf_iter_run_prog(prog, &ctx);
}

/* The only function that becomes extern from static. Don't bother to place it in header file. */
void part_stat_read_all(struct block_device *part, struct disk_stats *stat);

static int diskstats_show(struct seq_file *seqf, void *v)
{
	struct gendisk *gp = v;
	struct block_device *hd;
	unsigned int inflight;
	struct disk_stats stat;
	unsigned long idx;
	int ret = 0;

	rcu_read_lock();
	xa_for_each(&gp->part_tbl, idx, hd) {
		if (bdev_is_partition(hd) && !bdev_nr_sectors(hd))
			continue;
		if (queue_is_mq(gp->queue))
			inflight = blk_mq_in_flight(gp->queue, hd);
		else
			inflight = part_in_flight(hd);

		if (inflight) {
			part_stat_lock();
			update_io_ticks(hd, jiffies, true);
			part_stat_unlock();
		}
		part_stat_read_all(hd, &stat);
		ret = __diskstats_show(seqf, hd, &stat, inflight);
		if (ret)
			break;
	}
	rcu_read_unlock();

	return ret;
}

static const struct seq_operations bpf_diskstats_op = {
	.start	= bpf_disk_seqf_start,
	.next	= bpf_disk_seqf_next,
	.stop	= bpf_disk_seqf_stop,
	.show	= diskstats_show
};

static const struct bpf_iter_seq_info diskstats_seq_info = {
	.seq_ops		= &bpf_diskstats_op,
	.init_seq_private	= NULL,
	.fini_seq_private	= NULL,
	.seq_priv_size		= sizeof(struct diskstats_seq_priv),
};

static struct bpf_iter_reg diskstats_reg_info = {
	.target			= "diskstats",
	.ctx_arg_info_size	= 2,
	.ctx_arg_info		= {
		{ offsetof(struct bpf_iter__diskstats, bd),
		  PTR_TO_BTF_ID },
		{ offsetof(struct bpf_iter__diskstats, native_stat),
		  PTR_TO_BTF_ID },
	},
	.seq_info		= &diskstats_seq_info,
};

BTF_ID_LIST(btf_diststats_ids)
BTF_ID(struct, block_device)
BTF_ID(struct, disk_stats)

static int __init diskstats_iter_init(void)
{
	diskstats_reg_info.ctx_arg_info[0].btf_id = btf_diststats_ids[0];
	diskstats_reg_info.ctx_arg_info[1].btf_id = btf_diststats_ids[1];
	return bpf_iter_reg_target(&diskstats_reg_info);
}
late_initcall(diskstats_iter_init);
