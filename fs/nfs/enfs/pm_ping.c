// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */

#include "pm_ping.h"
#include <linux/err.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/kthread.h>
#include <linux/nfs.h>
#include <linux/errno.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>
#include <net/netns/generic.h>
#include <linux/atomic.h>
#include <linux/sunrpc/clnt.h>

#include "../../../net/sunrpc/netns.h"
#include "pm_state.h"
#include "enfs.h"
#include "enfs_log.h"
#include "enfs_config.h"
#include "enfs_tp_common.h"

#define SLEEP_INTERVAL 2

static struct task_struct *pm_ping_timer_thread;
static spinlock_t ping_execute_workq_lock;	//protect pint_execute_workq
static struct workqueue_struct *ping_execute_workq;	// timer for test xprt workqueue
static atomic_t check_xprt_count;	// count the ping xprt work on flight

struct ping_xprt_work {
	struct rpc_xprt *xprt;	// use this specific xprt
	struct rpc_clnt *clnt;	// use this specific rpc_client
	struct work_struct ping_work;
};

struct pm_ping_async_callback {
	void *data;
	void (*func)(void *data);
};

// set xprt's enfs_pm_check_state
void pm_ping_set_path_check_state(struct rpc_xprt *xprt, enum enfs_pm_check_state state)
{
	struct enfs_xprt_context *ctx = NULL;

	if (IS_ERR(xprt)) {
		enfs_log_error("The xprt ptr is not exist.\n");
		return;
	}

	if (xprt == NULL) {
		enfs_log_error("The xprt is not valid.\n");
		return;
	}

	xprt_get(xprt);

	ctx = (struct enfs_xprt_context *)xprt_get_reserve_context(xprt);
	if (ctx == NULL) {
		enfs_log_error("The xprt multipath ctx is not valid.\n");
		xprt_put(xprt);
		return;
	}

	atomic_set(&ctx->path_check_state, state);
	xprt_put(xprt);

}

// get xprt's enfs_pm_check_state
static enum enfs_pm_check_state pm_ping_get_path_check_state(struct rpc_xprt *xprt)
{
	struct enfs_xprt_context *ctx = NULL;
	enum enfs_pm_check_state state;

	if (xprt == NULL) {
		enfs_log_error("The xprt is not valid.\n");
		return PM_CHECK_UNDEFINE;
	}

	ctx = (struct enfs_xprt_context *)xprt_get_reserve_context(xprt);
	if (ctx == NULL) {
		enfs_log_error("The xprt multipath ctx is not valid.\n");
		return PM_CHECK_UNDEFINE;
	}

	state = atomic_read(&ctx->path_check_state);

	return state;
}

static void pm_ping_call_done_callback(void *data)
{
	struct pm_ping_async_callback *callback_data =
		(struct pm_ping_async_callback *)data;
	if (callback_data == NULL)
		return;
	callback_data->func(callback_data->data);

	kfree(callback_data);
}

static void set_xprt_close_wait(struct rpc_xprt *xprt)
{
	if (xprt == NULL) {
		enfs_log_error("The xprt is not valid.\n");
		return;
	}

	xprt_get(xprt);
	set_bit(XPRT_CLOSE_WAIT, &xprt->state);
	xprt_put(xprt);

}

// Default callback for async RPC calls
static void pm_ping_call_done(struct rpc_task *task, void *data)
{
	struct rpc_xprt *xprt = task->tk_xprt;
	struct enfs_xprt_context *ctx = NULL;
	ktime_t ktime;

	atomic_dec(&check_xprt_count);
	if (task->tk_status >= 0) {
		// xprt is not used,just match paramete.
		LVOS_TP_START(PM_ALLOC_WORK_INFO_FAILED, &xprt);
		pm_set_path_state(xprt, PM_STATE_NORMAL);
		LVOS_TP_END;
	} else {
		set_xprt_close_wait(xprt);
		pm_set_path_state(xprt, PM_STATE_FAULT);
	}
	pm_ping_set_path_check_state(xprt, PM_CHECK_FINISH);

	pm_ping_call_done_callback(data);

	xprt_get(xprt);
	ctx = (struct enfs_xprt_context *)xprt_get_reserve_context(xprt);
	if (ctx == NULL) {
		enfs_log_error("The xprt multipath ctx is not valid.\n");
		xprt_put(xprt);
		return;
	}
	ktime = ktime_get();
	ctx->lastTime = ktime_to_ms(ktime);
	xprt_put(xprt);
}

// register func to rpc_call_done
static const struct rpc_call_ops pm_ping_set_status_ops = {
	.rpc_call_done = pm_ping_call_done,
};

// execute work which in work_queue
static void pm_ping_execute_work(struct work_struct *work)
{
	int ret = 0;

	// get the work information
	struct ping_xprt_work *work_info =
		container_of(work, struct ping_xprt_work, ping_work);

	// if check state is pending
	if (pm_ping_get_path_check_state(work_info->xprt) == PM_CHECK_WAITING) {
		pm_ping_set_path_check_state(work_info->xprt,
						 PM_CHECK_CHECKING);
		ret = rpc_clnt_test_xprt(work_info->clnt, work_info->xprt,
					 &pm_ping_set_status_ops, NULL,
					 RPC_TASK_ASYNC | RPC_TASK_FIXED);
		if (ret != 1) {
			enfs_log_debug("ping xprt execute failed ,ret %d", ret);
			pm_ping_set_path_check_state(work_info->xprt,
							 PM_CHECK_FINISH);
		} else {
			atomic_inc(&check_xprt_count);
		}
	}

	rpc_release_client(work_info->clnt);
	xprt_put(work_info->xprt);
	kfree(work_info);
	work_info = NULL;
}

static bool pm_ping_workqueue_queue_work(struct work_struct *work)
{
	bool ret = false;

	spin_lock(&ping_execute_workq_lock);

	if (ping_execute_workq != NULL)
		ret = queue_work(ping_execute_workq, work);
	spin_unlock(&ping_execute_workq_lock);
	return ret;
}

// init test work and add this work to workqueue
static int pm_ping_add_work(struct rpc_clnt *clnt, struct rpc_xprt *xprt,
				void *data)
{
	struct list_head *head = data;
	struct rpcclnt_release_item *item;
	struct ping_xprt_work *work_info;
	struct enfs_xprt_context *ctx = NULL;
	bool ret = false;

	if (IS_ERR(xprt) || xprt == NULL) {
		enfs_log_error("The xprt ptr is not exist.\n");
		return -EINVAL;
	}

	if (IS_ERR(clnt) || clnt == NULL) {
		enfs_log_error("The clnt ptr is not exist.\n");
		return -EINVAL;
	}

	if (!xprt_get_reserve_context(xprt))
		return -EINVAL;

	ctx = (struct enfs_xprt_context *)xprt_get_reserve_context(xprt);
	if (ctx == NULL) {
		enfs_log_error("The xprt multipath ctx is not valid.\n");
		return -EINVAL;
	}

	if (!enfs_timeout_ms
		((ktime_t *) &ctx->lastTime, ENFS_PM_PING_TMIE_OUT * 1000)) {
		return 0;
	}

	if (pm_ping_get_path_check_state(xprt) == PM_CHECK_FINISH ||
		pm_ping_get_path_check_state(xprt) == PM_CHECK_INIT) {
		/* check xprt pending status, if pending status equals Finish,
		 * means this xprt can inster to work queue
		 */
		enfs_log_debug("find xprt pointer.   %p\n", xprt);
		work_info = kzalloc(sizeof(struct ping_xprt_work), GFP_ATOMIC);
		if (work_info == NULL)
			return -ENOMEM;

		item = kzalloc(sizeof(*item), GFP_KERNEL);
		if (!item) {
			enfs_log_error("alloc item failed.\n");
			kfree(work_info);
			return -ENOMEM;
		}

		work_info->clnt = clnt;
		work_info->xprt = xprt;
		xprt_get(xprt);
		INIT_WORK(&work_info->ping_work, pm_ping_execute_work);
		pm_ping_set_path_check_state(xprt, PM_CHECK_WAITING);
		if (!refcount_inc_not_zero(&clnt->cl_count)) {
			xprt_put(work_info->xprt);
			kfree(item);
			kfree(work_info);
			return 0;
		}

		ret = pm_ping_workqueue_queue_work(&work_info->ping_work);
		if (!ret) {
			item->clnt = work_info->clnt;
			list_add_tail(&item->node, head);
			xprt_put(work_info->xprt);
			kfree(work_info);
			return -EINVAL;
		}

		/* Note:that normal exit requires release. */
		kfree(item);
	}
	return 0;
}

// encapsulate pm_ping_add_work()
static int pm_ping_execute_xprt_test(struct rpc_clnt *clnt,
					 struct rpc_xprt *xprt, void *data)
{

	pm_ping_add_work(clnt, xprt, data);
	// return 0 for rpc_clnt_iterate_for_each_xprt(clnt,
	// pm_ping_execute_xprt_test, NULL); because negative value will stop
	// iterate all xprt and we need return negative value for debug Therefore,
	// we need this function to iterate all xprt
	return 0;
}

// iterate xprt in the client
static void pm_ping_loop_rpclnt(struct sunrpc_net *sn)
{
	struct rpc_clnt *clnt;
	struct rpc_clnt_reserve *clnt_reserve;
	LIST_HEAD(free_list);

	spin_lock(&sn->rpc_client_lock);
	list_for_each_entry_rcu(clnt, &sn->all_clients, cl_clients) {
		clnt_reserve = (struct rpc_clnt_reserve *)clnt;
		if (clnt_reserve->cl_enfs == 1) {
			enfs_log_debug("find rpc_clnt.   %p\n", clnt);
			rpc_clnt_iterate_for_each_xprt(clnt, pm_ping_execute_xprt_test,
							   (void *)&free_list);
		}
	}
	spin_unlock(&sn->rpc_client_lock);
	enfs_destroy_rpcclnt_list(&free_list);
}

// iterate each clnt in the sunrpc_net
static void pm_ping_loop_sunrpc_net(void)
{
	struct net *net;
	struct sunrpc_net *sn;

	rcu_read_lock();
	for_each_net_rcu(net) {
		sn = net_generic(net, sunrpc_net_id);
		if (sn == NULL)
			continue;
		pm_ping_loop_rpclnt(sn);
	}
	rcu_read_unlock();
}

static int pm_ping_routine(void *data)
{
	ktime_t start = ktime_get();
	int32_t interval_ms;

	while (!kthread_should_stop()) {
		interval_ms = enfs_get_config_path_detect_interval() * 1000;
		if (enfs_get_config_multipath_state() == ENFS_MULTIPATH_ENABLE
			&& enfs_timeout_ms(&start, interval_ms)) {
			start = ktime_get();
			pm_ping_loop_sunrpc_net();
		}
		enfs_msleep(1000);
	}
	return 0;
}

// start thread to cycly ping
static int pm_ping_start(void)
{
	pm_ping_timer_thread =
		kthread_run(pm_ping_routine, NULL, "pm_ping_routine");
	if (IS_ERR(pm_ping_timer_thread)) {
		enfs_log_error("Failed to create kernel thread\n");
		return PTR_ERR(pm_ping_timer_thread);
	}
	return 0;
}

// initialize workqueue
static int pm_ping_workqueue_init(void)
{
	struct workqueue_struct *queue = NULL;

	queue = create_workqueue("pm_ping_workqueue");
	if (queue == NULL) {
		enfs_log_error("create workqueue failed.\n");
		return -ENOMEM;
	}

	spin_lock(&ping_execute_workq_lock);
	ping_execute_workq = queue;
	spin_unlock(&ping_execute_workq_lock);
	enfs_log_info("create workqueue succeeded.\n");
	return 0;
}

static void pm_ping_workqueue_fini(void)
{
	struct workqueue_struct *queue = NULL;

	spin_lock(&ping_execute_workq_lock);
	queue = ping_execute_workq;
	ping_execute_workq = NULL;
	spin_unlock(&ping_execute_workq_lock);

	enfs_log_info("delete work queue\n");

	if (queue != NULL) {
		flush_workqueue(queue);
		destroy_workqueue(queue);
	}
}

// module exit func
void pm_ping_fini(void)
{
	if (pm_ping_timer_thread)
		kthread_stop(pm_ping_timer_thread);

	pm_ping_workqueue_fini();

	while (atomic_read(&check_xprt_count) != 0)
		enfs_msleep(SLEEP_INTERVAL);

}

// module init func
int pm_ping_init(void)
{
	int ret;

	spin_lock_init(&ping_execute_workq_lock);
	atomic_set(&check_xprt_count, 0);
	ret = pm_ping_workqueue_init();
	if (ret != 0) {
		enfs_log_error("PM_PING Module loading failed.\n");
		return ret;
	}
	ret = pm_ping_start();
	if (ret != 0) {
		enfs_log_error("PM_PING Module loading failed.\n");
		pm_ping_workqueue_fini();
		return ret;
	}

	return ret;
}

bool pm_ping_is_test_xprt_task(struct rpc_task *task)
{
	return task->tk_ops == &pm_ping_set_status_ops ? true : false;
}

int pm_ping_rpc_test_xprt_with_callback(struct rpc_clnt *clnt,
					struct rpc_xprt *xprt,
					void (*func)(void *data), void *data)
{
	int ret;
	struct pm_ping_async_callback *callback_data =
		(struct pm_ping_async_callback *)
		kzalloc(sizeof(struct pm_ping_async_callback), GFP_KERNEL);
	if (callback_data == NULL) {
		enfs_log_error("failed to mzalloc mem\n");
		return -ENOMEM;
	}

	callback_data->data = data;
	callback_data->func = func;
	atomic_inc(&check_xprt_count);
	ret =
		rpc_clnt_test_xprt(clnt, xprt, &pm_ping_set_status_ops,
				   callback_data, RPC_TASK_ASYNC | RPC_TASK_FIXED);
	if (ret != 1) {
		enfs_log_debug("ping xprt execute failed ,ret %d", ret);
		atomic_dec(&check_xprt_count);
	}

	return ret;
}
