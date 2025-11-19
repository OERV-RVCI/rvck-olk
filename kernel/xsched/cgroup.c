// SPDX-License-Identifier: GPL-2.0+
/*
 * Support cgroup for xpu device
 *
 * Copyright (C) 2025-2026 Huawei Technologies Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */
#include <linux/err.h>
#include <linux/cgroup.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/xsched.h>

enum xcu_file_type {
	XCU_FILE_PERIOD_MS,
	XCU_FILE_QUOTA_MS,
	XCU_FILE_SHARES,
};

static struct xsched_group root_xsched_group;
struct xsched_group *root_xcg = &root_xsched_group;

/*
 * Cacheline aligned slab cache for xsched_group,
 * to replace kzalloc with kmem_cache_alloc.
 */
static struct kmem_cache *xsched_group_cache __read_mostly;
static struct kmem_cache *xcg_attach_entry_cache __read_mostly;
static LIST_HEAD(xcg_attach_list);

static const char xcu_sched_name[XSCHED_TYPE_NUM][4] = {
	[XSCHED_TYPE_RT] = "rt",
	[XSCHED_TYPE_CFS] = "cfs"
};

/**
 * @brief Initialize the core components of an xsched_group.
 *
 * This function initializes the essential components of an xsched_group,
 * including the spin lock, member list, children groups list, quota timeout
 * mechanism, and refill work queue. These components are necessary for the
 * proper functioning of the xsched_group.
 *
 * @param xcg Pointer to the xsched_group to be initialized.
 */
static void xcu_cg_initialize_components(struct xsched_group *xcg)
{
	spin_lock_init(&xcg->lock);
	INIT_LIST_HEAD(&xcg->members);
	INIT_LIST_HEAD(&xcg->children_groups);
}

void xcu_cg_subsys_init(void)
{
	xcu_cg_initialize_components(root_xcg);

	root_xcg->sched_class = XSCHED_TYPE_DFLT;

	xsched_group_cache = KMEM_CACHE(xsched_group, 0);
	xcg_attach_entry_cache = KMEM_CACHE(xcg_attach_entry, 0);
}

void xcu_cfs_root_cg_init(struct xsched_cu *xcu)
{
	int id = xcu->id;

	root_xcg->perxcu_priv[id].xcu_id = id;
	root_xcg->perxcu_priv[id].self = root_xcg;
	root_xcg->perxcu_priv[id].cfs_rq = &xcu->xrq.cfs;
	root_xcg->perxcu_priv[id].xse.cfs.weight = XSCHED_CFS_WEIGHT_DFLT;
}

/**
 * xcu_cfs_cg_init() - Initialize xsched_group cfs runqueues and bw control.
 * @xcg: new xsched_cgroup
 * @parent_xg: parent's group
 *
 * One xsched_group can host many processes with contexts on different devices.
 * Function creates xsched_entity for every XCU, and places it in runqueue
 * of parent group. Create new cfs rq for xse inside group.
 */
static int xcu_cfs_cg_init(struct xsched_group *xcg,
				struct xsched_group *parent_xg)
{
	int id = 0, err, i;
	struct xsched_cu *xcu;
	struct xsched_rq_cfs *sub_cfs_rq;

	for_each_active_xcu(xcu, id) {
		xcg->perxcu_priv[id].xcu_id = id;
		xcg->perxcu_priv[id].self = xcg;

		sub_cfs_rq = kzalloc(sizeof(struct xsched_rq_cfs), GFP_KERNEL);
		if (!sub_cfs_rq) {
			XSCHED_ERR("Fail to alloc cfs runqueue on xcu %d\n", id);
			err = -ENOMEM;
			goto alloc_error;
		}
		xcg->perxcu_priv[id].cfs_rq = sub_cfs_rq;
		xcg->perxcu_priv[id].cfs_rq->ctx_timeline = RB_ROOT_CACHED;

		xcg->perxcu_priv[id].xse.is_group = true;
		xcg->perxcu_priv[id].xse.xcu = xcu;
		xcg->perxcu_priv[id].xse.class = &fair_xsched_class;

		/* Put new empty groups to the right in parent's rbtree: */
		xcg->perxcu_priv[id].xse.cfs.xruntime = XSCHED_TIME_INF;
		xcg->perxcu_priv[id].xse.cfs.weight = XSCHED_CFS_WEIGHT_DFLT;
		xcg->perxcu_priv[id].xse.parent_grp = parent_xg;

		mutex_lock(&xcu->xcu_lock);
		enqueue_ctx(&xcg->perxcu_priv[id].xse, xcu);
		mutex_unlock(&xcu->xcu_lock);
	}

	xcg->shares_cfg = XSCHED_CFG_SHARE_DFLT;
	xcu_grp_shares_update(parent_xg);

	return 0;

alloc_error:
	for (i = 0; i < id; i++) {
		xcu = xsched_cu_mgr[i];
		mutex_lock(&xcu->xcu_lock);
		dequeue_ctx(&xcg->perxcu_priv[i].xse, xcu);
		mutex_unlock(&xcu->xcu_lock);

		kfree(xcg->perxcu_priv[i].cfs_rq);
	}

	return err;
}

static void xcu_cfs_cg_deinit(struct xsched_group *xcg)
{
	uint32_t id;
	struct xsched_cu *xcu;

	for_each_active_xcu(xcu, id) {
		mutex_lock(&xcu->xcu_lock);
		dequeue_ctx(&xcg->perxcu_priv[id].xse, xcu);
		mutex_unlock(&xcu->xcu_lock);
		kfree(xcg->perxcu_priv[id].cfs_rq);
	}
	xcu_grp_shares_update(xcg->parent);
}

/**
 * xcu_cg_init() - Initialize non-root xsched_group structure.
 * @xcg: new xsched_cgroup
 * @parent_xg: parent's group
 */
static int xcu_cg_init(struct xsched_group *xcg,
				struct xsched_group *parent_xg)
{
	xcu_cg_initialize_components(xcg);
	xcg->parent = parent_xg;
	list_add_tail(&xcg->group_node, &parent_xg->children_groups);
	xcg->sched_class = parent_xg->sched_class;

	switch (xcg->sched_class) {
	case XSCHED_TYPE_CFS:
		return xcu_cfs_cg_init(xcg, parent_xg);
	default:
		XSCHED_INFO("xcu_cgroup: init RT group css=0x%lx\n",
		       (uintptr_t)&xcg->css);
		break;
	}

	return 0;
}

inline struct xsched_group *xcu_cg_from_css(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct xsched_group, css) : NULL;
}

/*
 * Determine whether the given css corresponds to root_xsched_group.css.
 *
 * Parameter only_css_self:
 *   - true  : Only check whether the css pointer itself is NULL
 *             (i.e., the subsystem root). Do not dereference xg->parent.
 *             Used in the allocation path (css_alloc).
 *   - false : Further check whether the associated xsched_group
 *             has no parent (i.e., a normal root check).
 */
static inline bool xsched_group_css_is_root(struct cgroup_subsys_state *css, bool only_css_self)
{
	struct xsched_group *xg;

	/* NULL indicates the subsystem root */
	if (!css)
		return true;

	/*
	 * During the allocation phase,
	 * cannot find its parent xsched_group via xg->parent,
	 * so can only determine on the css itself.
	 */
	if (only_css_self)
		return false;

	xg = xcu_cg_from_css(css);

	return xg && !xg->parent;
}

/**
 * xcu_css_alloc() - Allocate and init xcu cgroup.
 * @parent_css: css of parent xcu cgroup
 *
 * Called from kernel/cgroup.c with cgroup_lock() held.
 * First called in subsys initialization to create root xcu cgroup, when
 * XCUs haven't been initialized yet. Func used on every new cgroup creation,
 * on second call to set root xsched_group runqueue.
 *
 * Return: pointer of new xcu cgroup css on success, -ENOMEM otherwise.
 */
static struct cgroup_subsys_state *
xcu_css_alloc(struct cgroup_subsys_state *parent_css)
{
	struct xsched_group *xg;

	if (xsched_group_css_is_root(parent_css, true))
		return &root_xsched_group.css;

	xg = kmem_cache_alloc(xsched_group_cache, GFP_KERNEL | __GFP_ZERO);
	if (!xg)
		return ERR_PTR(-ENOMEM);

	return &xg->css;
}

static void xcu_css_free(struct cgroup_subsys_state *css)
{
	struct xsched_group *xcg = xcu_cg_from_css(css);

	kmem_cache_free(xsched_group_cache, xcg);
}

static int xcu_css_online(struct cgroup_subsys_state *css)
{
	struct xsched_group *xg = xcu_cg_from_css(css);
	struct cgroup_subsys_state *parent_css = css->parent;
	struct xsched_group *parent_xg;
	int err;

	if (!parent_css)
		return 0;

	parent_xg = xcu_cg_from_css(parent_css);
	err = xcu_cg_init(xg, parent_xg);
	if (err) {
		kmem_cache_free(xsched_group_cache, xg);
		XSCHED_ERR("Failed to initialize new xsched_group @ %s.\n", __func__);
		return err;
	}

	return 0;
}

static void xcu_css_offline(struct cgroup_subsys_state *css)
{
	struct xsched_group *xcg;

	xcg = xcu_cg_from_css(css);
	if (!xsched_group_css_is_root(css, false)) {
		switch (xcg->sched_class) {
		case XSCHED_TYPE_CFS:
			xcu_cfs_cg_deinit(xcg);
			break;
		default:
			XSCHED_INFO("xcu_cgroup: deinit RT group css=0x%lx\n",
			       (uintptr_t)&xcg->css);
			break;
		}
	}
	list_del(&xcg->group_node);
}

static void xsched_group_xse_attach(struct xsched_group *xg,
				struct xsched_entity *xse)
{
	spin_lock(&xg->lock);
	list_add_tail(&xse->group_node, &xg->members);
	spin_unlock(&xg->lock);
	xse->parent_grp = xg;
}

void xsched_group_xse_detach(struct xsched_entity *xse)
{
	struct xsched_group *xcg = xse->parent_grp;

	spin_lock(&xcg->lock);
	list_del(&xse->group_node);
	spin_unlock(&xcg->lock);
}

static int xcu_task_can_attach(struct task_struct *task,
			struct xsched_group *old)
{
	struct xsched_entity *xse;
	bool has_xse = false;

	spin_lock(&old->lock);
	list_for_each_entry(xse, &old->members, group_node) {
		if (xse->owner_pid == task_pid_nr(task)) {
			has_xse = true;
			break;
		}
	}
	spin_unlock(&old->lock);

	return has_xse ? -EINVAL : 0;
}

static int xcu_can_attach(struct cgroup_taskset *tset)
{
	struct task_struct *task;
	struct cgroup_subsys_state *dst_css, *old_css;
	struct xsched_group *old_xcg, *dst_xcg;
	struct xcg_attach_entry *entry;
	int ret = 0;

	cgroup_taskset_for_each(task, dst_css, tset) {
		rcu_read_lock();
		old_css = task_css(task, xcu_cgrp_id);
		rcu_read_unlock();
		dst_xcg = xcu_cg_from_css(dst_css);
		old_xcg = xcu_cg_from_css(old_css);

		ret = xcu_task_can_attach(task, old_xcg);
		if (ret)
			break;

		/* record entry for this task */
		entry = kmem_cache_alloc(xcg_attach_entry_cache, GFP_KERNEL | __GFP_ZERO);
		entry->task = task;
		entry->old_xcg = old_xcg;
		entry->new_xcg = dst_xcg;
		list_add_tail(&entry->node, &xcg_attach_list);
	}

	return ret;
}

static void xcu_cancel_attach(struct cgroup_taskset *tset)
{
	struct xcg_attach_entry *entry, *tmp;

	/* error: clear all entries */
	list_for_each_entry_safe(entry, tmp, &xcg_attach_list, node) {
		list_del(&entry->node);
		kmem_cache_free(xcg_attach_entry_cache, entry);
	}
}

void xcu_move_task(struct task_struct *task, struct xsched_group *old_xcg,
			struct xsched_group *new_xcg)
{
	struct xsched_entity *xse, *tmp;
	struct xsched_cu *xcu;

	spin_lock(&old_xcg->lock);
	list_for_each_entry_safe(xse, tmp, &old_xcg->members, group_node) {
		if (xse->owner_pid != task_pid_nr(task))
			continue;

		xcu = xse->xcu;

		if (old_xcg != xse->parent_grp) {
			WARN_ON(old_xcg != xse->parent_grp);
			return;
		}

		/* delete from the old_xcg */
		list_del(&xse->group_node);

		mutex_lock(&xcu->xcu_lock);
		/* dequeue from the current runqueue */
		dequeue_ctx(xse, xcu);
		/* attach to the new_xcg */
		xsched_group_xse_attach(new_xcg, xse);
		/* enqueue to the runqueue in new_xcg */
		enqueue_ctx(xse, xcu);
		mutex_unlock(&xcu->xcu_lock);
	}
	spin_unlock(&old_xcg->lock);
}

static void xcu_attach(struct cgroup_taskset *tset)
{
	struct xcg_attach_entry *entry, *tmp;

	list_for_each_entry(entry, &xcg_attach_list, node) {
		xcu_move_task(entry->task, entry->old_xcg, entry->new_xcg);
	}

	/* cleanup */
	list_for_each_entry_safe(entry, tmp, &xcg_attach_list, node) {
		list_del(&entry->node);
		kmem_cache_free(xcg_attach_entry_cache, entry);
	}
}

/**
 * xsched_group_inherit() - Attach new entity to task's xsched_group.
 * @task: task_struct
 * @xse: xsched entity
 *
 * Called in xsched context initialization to attach xse to task's group
 * and inherit its xse scheduling class and bandwidth control policy.
 *
 * Return: Zero on success.
 */
void xsched_group_inherit(struct task_struct *task, struct xsched_entity *xse)
{
	struct cgroup_subsys_state *css;
	struct xsched_group *xg;

	xse->owner_pid = task_pid_nr(task);
	css = task_get_css(task, xcu_cgrp_id);
	xg = xcu_cg_from_css(css);
	xsched_group_xse_attach(xg, xse);
	css_put(css);
}

static int xcu_sched_class_show(struct seq_file *sf, void *v)
{
	struct cgroup_subsys_state *css = seq_css(sf);
	struct xsched_group *xg = xcu_cg_from_css(css);

	seq_printf(sf, "%s\n", xcu_sched_name[xg->sched_class]);
	return 0;
}

/**
 * xcu_cg_set_sched_class() - Set scheduling type for group.
 * @xg: xsched group
 * @type: scheduler type
 *
 * Scheduler type can be changed if task is child of root group
 * and haven't got scheduling entities.
 *
 * Return: Zero on success or -EINVAL
 */
static int xcu_cg_set_sched_class(struct xsched_group *xg, int type)
{
	if (type == xg->sched_class)
		return 0;

	/* can't change scheduler when there are running members */
	if (!list_empty(&xg->members))
		return -EBUSY;

	/* deinit old type if necessary */
	switch (xg->sched_class) {
	case XSCHED_TYPE_CFS:
		xcu_cfs_cg_deinit(xg);
		break;
	default:
		break;
	}

	/* update type */
	xg->sched_class = type;

	/* init new type if necessary */
	switch (type) {
	case XSCHED_TYPE_CFS:
		return xcu_cfs_cg_init(xg, xg->parent);
	default:
		return 0;
	}
}

static ssize_t xcu_sched_class_write(struct kernfs_open_file *of, char *buf,
				size_t nbytes, loff_t off)
{
	struct cgroup_subsys_state *css = of_css(of);
	struct xsched_group *xg = xcu_cg_from_css(css);
	char type_name[4];
	int type = -1;

	ssize_t ret = sscanf(buf, "%3s", type_name);

	if (ret < 1)
		return -EINVAL;

	for (type = 0; type < XSCHED_TYPE_NUM; type++) {
		if (!strcmp(type_name, xcu_sched_name[type]))
			break;
	}

	if (type == XSCHED_TYPE_NUM)
		return -EINVAL;

	if (!list_empty(&css->children))
		return -EBUSY;

	/* only root child can switch scheduler type */
	if (!xg->parent || !xsched_group_css_is_root(&xg->parent->css, false))
		return -EINVAL;

	ret = xcu_cg_set_sched_class(xg, type);

	return (ret) ? ret : nbytes;
}

static s64 xcu_read_s64(struct cgroup_subsys_state *css, struct cftype *cft)
{
	s64 ret = 0;
	struct xsched_group *xcucg = xcu_cg_from_css(css);

	switch (cft->private) {
	case XCU_FILE_SHARES:
		ret = xcucg->shares_cfg;
		break;
	default:
		XSCHED_ERR("invalid operation %lu @ %s\n", cft->private, __func__);
		break;
	}

	return ret;
}

void xcu_grp_shares_update(struct xsched_group *parent)
{
	int id;
	struct xsched_cu *xcu;
	struct xsched_group *children;
	u64 rem, sh_sum = 0, sh_gcd = 0, w_gcd = 0, sh_prod_red = 1;

	spin_lock(&parent->lock);
	list_for_each_entry(children, &(parent)->children_groups, group_node) {
		if (children->sched_class == XSCHED_TYPE_CFS)
			sh_gcd = gcd(sh_gcd, children->shares_cfg);
	}

	list_for_each_entry(children, &(parent)->children_groups, group_node) {
		if (children->sched_class == XSCHED_TYPE_CFS) {
			sh_sum += children->shares_cfg;
			children->shares_cfg_red = div64_u64(children->shares_cfg, sh_gcd);
			div64_u64_rem(sh_prod_red, children->shares_cfg_red, &rem);
			if (rem)
				sh_prod_red *= children->shares_cfg_red;
		}
	}

	parent->children_shares_sum = sh_sum;
	list_for_each_entry(children, &(parent)->children_groups, group_node) {
		if (children->sched_class == XSCHED_TYPE_CFS) {
			children->weight = div64_u64(sh_prod_red, children->shares_cfg_red);
			w_gcd = gcd(w_gcd, children->weight);
		}
	}

	list_for_each_entry(children, &(parent)->children_groups, group_node) {
		if (children->sched_class == XSCHED_TYPE_CFS) {
			children->weight = div64_u64(children->weight, w_gcd);
			for_each_active_xcu(xcu, id) {
				mutex_lock(&xcu->xcu_lock);
				children->perxcu_priv[id].xse.cfs.weight = children->weight;
				mutex_unlock(&xcu->xcu_lock);
			}
		}
	}
	spin_unlock(&parent->lock);
}

static int xcu_write_s64(struct cgroup_subsys_state *css, struct cftype *cft,
			s64 val)
{
	int ret = 0;
	struct xsched_group *xcucg = xcu_cg_from_css(css);

	switch (cft->private) {
	case XCU_FILE_SHARES:
		if (val <= 0) {
			ret = -EINVAL;
			break;
		}
		xcucg->shares_cfg = val;
		xcu_grp_shares_update(xcucg->parent);
		break;
	default:
		XSCHED_ERR("invalid operation %lu @ %s\n", cft->private, __func__);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int xcu_stat(struct seq_file *sf, void *v)
{
	struct cgroup_subsys_state *css = seq_css(sf);
	struct xsched_group *xcucg = xcu_cg_from_css(css);
	u64 exec_runtime = 0;
	int xcu_id;
	struct xsched_cu *xcu;

	if (xcucg->sched_class == XSCHED_TYPE_RT) {
		seq_printf(sf, "RT group stat is not supported @ %s.\n", __func__);
		return 0;
	}

	for_each_active_xcu(xcu, xcu_id) {
		exec_runtime +=
			xcucg->perxcu_priv[xcu_id].xse.cfs.sum_exec_runtime;
	}

	seq_printf(sf, "exec_runtime:	%llu\n", exec_runtime);
	seq_printf(sf, "shares cfg:	%llu/%llu x%u\n", xcucg->shares_cfg,
		   xcucg->parent->children_shares_sum, xcucg->weight);

	return 0;
}

static struct cftype xcu_cg_files[] = {
	{
		.name = "shares",
		.flags = CFTYPE_NOT_ON_ROOT,
		.read_s64 = xcu_read_s64,
		.write_s64 = xcu_write_s64,
		.private = XCU_FILE_SHARES,
	},
	{
		.name = "stat",
		.seq_show = xcu_stat,
	},
	{
		.name = "sched_class",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = xcu_sched_class_show,
		.write = xcu_sched_class_write,
	},
	{} /* terminate */
};

struct cgroup_subsys xcu_cgrp_subsys = {
	.css_alloc = xcu_css_alloc,
	.css_online = xcu_css_online,
	.css_offline = xcu_css_offline,
	.css_free = xcu_css_free,
	.can_attach = xcu_can_attach,
	.cancel_attach = xcu_cancel_attach,
	.attach = xcu_attach,
	.dfl_cftypes = xcu_cg_files,
	.legacy_cftypes = xcu_cg_files,
	.early_init = false,
};
