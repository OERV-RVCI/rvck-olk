/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_XSCHED_H__
#define __LINUX_XSCHED_H__

#include <linux/kref.h>
#include <linux/vstream.h>
#include <linux/xcu_group.h>

#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

#define XSCHED_INFO_PREFIX "XSched [INFO]: "
#define XSCHED_INFO(fmt, ...)                                                  \
	pr_info(pr_fmt(XSCHED_INFO_PREFIX fmt), ##__VA_ARGS__)

#define XSCHED_ERR_PREFIX "XSched [ERROR]: "
#define XSCHED_ERR(fmt, ...)                                                   \
	pr_err(pr_fmt(XSCHED_ERR_PREFIX fmt), ##__VA_ARGS__)

#define XSCHED_WARN_PREFIX "XSched [WARNING]: "
#define XSCHED_WARN(fmt, ...)                                                  \
	pr_warn(pr_fmt(XSCHED_WARN_PREFIX fmt), ##__VA_ARGS__)

/*
 * Debug specific prints for XSched
 */

#define XSCHED_DEBUG_PREFIX "XSched [DEBUG]: "
#define XSCHED_DEBUG(fmt, ...)                                                 \
	pr_debug(pr_fmt(XSCHED_DEBUG_PREFIX fmt), ##__VA_ARGS__)

#define XSCHED_CALL_STUB()                                                     \
	XSCHED_DEBUG(" -----* %s @ %s called *-----\n", __func__, __FILE__)

#define XSCHED_EXIT_STUB()                                                     \
	XSCHED_DEBUG(" -----* %s @ %s exited *-----\n", __func__, __FILE__)

#define MAX_VSTREAM_NUM 512

#define RUNTIME_INF ((u64)~0ULL)
#define XSCHED_TIME_INF RUNTIME_INF
#define XSCHED_CFS_WEIGHT_DFLT 1

/*
 * A default kick slice for RT class XSEs.
 */
#define XSCHED_RT_KICK_SLICE 2
/*
 * A default kick slice for CFS class XSEs.
 */
#define XSCHED_CFS_KICK_SLICE 10

extern struct xsched_cu *xsched_cu_mgr[XSCHED_NR_CUS];

enum xcu_sched_type {
	XSCHED_TYPE_RT = 0,
	XSCHED_TYPE_CFS = 1,
	XSCHED_TYPE_NUM,
	XSCHED_TYPE_DFLT = XSCHED_TYPE_RT
};

enum xse_prio {
	XSE_PRIO_HIGH = 0,
	XSE_PRIO_LOW = 4,
	NR_XSE_PRIO,
	XSE_PRIO_DFLT = XSE_PRIO_LOW
};

extern struct xsched_class rt_xsched_class;
extern struct xsched_class fair_xsched_class;

#define xsched_first_class \
	list_first_entry(&(xsched_class_list), struct xsched_class, node)

#define for_each_xsched_class(class)                                           \
	list_for_each_entry((class), &(xsched_class_list), node)

#define for_each_xse_prio(prio)                                                \
	for (prio = XSE_PRIO_HIGH; prio < NR_XSE_PRIO; prio++)
#define for_each_vstream_in_ctx(vs, ctx)                                       \
	list_for_each_entry((vs), &((ctx)->vstream_list), ctx_node)


/* Manages xsched RT-like class linked list based runqueue.
 *
 * Now RT-like class runqueue structs is identical
 * but will most likely grow different in the
 * future as the Xsched evolves.
 */
struct xsched_rq_rt {
	struct list_head rq[NR_XSE_PRIO];
	unsigned int nr_running;
};

/* Manages xsched CFS-like class rbtree based runqueue. */
struct xsched_rq_cfs {
	unsigned int nr_running;
	unsigned int load;
	u64 min_xruntime;
	struct rb_root_cached ctx_timeline;
};

/* Base XSched runqueue object structure that contains both mutual and
 * individual parameters for different scheduling classes.
 */
struct xsched_rq {
	struct xsched_entity *curr_xse;
	const struct xsched_class *class;

	int state;
	int nr_running;
	/* RT class run queue.*/
	struct xsched_rq_rt rt;
	/* CFS class run queue.*/
	struct xsched_rq_cfs cfs;
};

enum xsched_cu_status {
	/* Worker not initialized. */
	XSCHED_XCU_NONE,

	/* Worker is sleeping in idle state. */
	XSCHED_XCU_WAIT_IDLE,

	/* Worker is sleeping in running state. */
	XSCHED_XCU_WAIT_RUNNING,

	/* Worker is active but not processing anything. */
	XSCHED_XCU_ACTIVE,

	NR_XSCHED_XCU_STATUS,
};

/* This is the abstraction object of the xcu computing unit. */
struct xsched_cu {
	uint32_t id;
	uint32_t state;

	atomic_t pending_kicks;
	struct task_struct *worker;

	/* Storage list for contexts associated with this xcu */
	uint32_t nr_ctx;
	struct list_head ctx_list;
	struct mutex ctx_list_lock;

	vstream_info_t *vs_array[MAX_VSTREAM_NUM];
	struct mutex vs_array_lock;

	struct xsched_rq xrq;
	struct list_head vsm_list;

	struct xcu_group *group;
	struct mutex xcu_lock;
	wait_queue_head_t wq_xcu_idle;
};

extern int num_active_xcu;
#define for_each_active_xcu(xcu, id)                                           \
	for ((id) = 0, xcu = xsched_cu_mgr[(id)];                                  \
	     (id) < num_active_xcu && (xcu = xsched_cu_mgr[(id)]); (id)++)

struct xsched_entity_rt {
	struct list_head list_node;
	enum xse_prio prio;

	ktime_t timeslice;
};

struct xsched_entity_cfs {
	struct rb_node run_node;

	/* Rq on which this entity is (to be) queued. */
	struct xsched_rq_cfs *cfs_rq;

	/* Value of "virtual" runtime to sort entities in rbtree */
	u64 xruntime;
	u32 weight;

	/* Execution time of scheduling entity */
	u64 exec_start;
	u64 sum_exec_runtime;
};

struct xsched_entity {
	uint32_t task_type;

	bool on_rq;

	pid_t owner_pid;
	pid_t tgid;

	/* Amount of pending kicks currently sitting on this context. */
	atomic_t kicks_pending_ctx_cnt;

	/* Amount of submitted kicks context, used for resched decision. */
	atomic_t submitted_one_kick;

	size_t total_scheduled;
	size_t total_submitted;

	/* File descriptor coming from an associated context
	 * used for identifying a given xsched entity in
	 * info and error prints.
	 */
	uint32_t fd;

	/* Xsched class for this xse. */
	const struct xsched_class *class;

	/* RT class entity. */
	struct xsched_entity_rt rt;
	/* CFS class entity. */
	struct xsched_entity_cfs cfs;

	/* Pointer to context object. */
	struct xsched_context *ctx;

	/* Xsched entity execution statistics */
	u64 last_exec_runtime;

	/* Pointer to an XCU object that represents an XCU
	 * on which this xse is to be processed or is being
	 * processed currently.
	 */
	struct xsched_cu *xcu;

	/* General purpose xse lock. */
	spinlock_t xse_lock;
};

/* Increments pending kicks counter for an XCU that the given
 * xsched entity is attached to and for xsched entity's xsched
 * class.
 */
static inline int xsched_inc_pending_kicks_xse(struct xsched_entity *xse)
{
	atomic_inc(&xse->xcu->pending_kicks);
	/* Icrement pending kicks for current XSE. */
	atomic_inc(&xse->kicks_pending_ctx_cnt);

	return 0;
}

/* Decrements pending kicks counter for an XCU that the given
 * xsched entity is attached to and for XSched entity's sched
 * class.
 */
static inline int xsched_dec_pending_kicks_xse(struct xsched_entity *xse)
{
	atomic_dec(&xse->xcu->pending_kicks);
	/* Decrementing pending kicks for current XSE. */
	atomic_dec(&xse->kicks_pending_ctx_cnt);

	return 0;
}

/* Checks if there are pending kicks left on a given XCU for all
 * xsched classes.
 */
static inline bool xsched_check_pending_kicks_xcu(struct xsched_cu *xcu)
{
	return atomic_read(&xcu->pending_kicks);
}

static inline int xse_integrity_check(const struct xsched_entity *xse)
{
	if (!xse) {
		XSCHED_ERR("xse is null @ %s\n", __func__);
		return -EINVAL;
	}

	if (!xse->class) {
		XSCHED_ERR("xse->class is null @ %s\n", __func__);
		return -EINVAL;
	}

	return 0;
}

struct xsched_context {
	uint32_t fd;
	uint32_t dev_id;
	pid_t tgid;

	struct list_head vstream_list;
	struct list_head ctx_node;

	struct xsched_entity xse;

	spinlock_t ctx_lock;
	struct mutex ctx_mutex;
	struct kref kref;
};

extern struct list_head xsched_ctx_list;
extern struct mutex xsched_ctx_list_mutex;

/* Returns a pointer to xsched_context object corresponding to a given
 * tgid and xcu.
 */
static inline struct xsched_context *
ctx_find_by_tgid_and_xcu(pid_t tgid, struct xsched_cu *xcu)
{
	struct xsched_context *ctx;
	struct xsched_context *ret = NULL;

	list_for_each_entry(ctx, &xcu->ctx_list, ctx_node) {
		if (ctx->tgid == tgid) {
			ret = ctx;
			break;
		}
	}
	return ret;
}

struct xsched_class {
	enum xcu_sched_type class_id;
	size_t kick_slice;
	struct list_head node;

	/* Initialize a new xsched entity */
	void (*xse_init)(struct xsched_entity *xse);

	/* Destroy XSE scheduler-specific data */
	void (*xse_deinit)(struct xsched_entity *xse);

	/* Initialize a new runqueue per xcu */
	void (*rq_init)(struct xsched_cu *xcu);

	/* Removes a given XSE from it's runqueue. */
	void (*dequeue_ctx)(struct xsched_entity *xse);

	/* Places a given XSE on a runqueue on a given XCU. */
	void (*enqueue_ctx)(struct xsched_entity *xse, struct xsched_cu *xcu);

	/* Returns a next XSE to be submitted on a given XCU. */
	struct xsched_entity *(*pick_next_ctx)(struct xsched_cu *xcu);

	/* Put a XSE back into rq during preemption. */
	void (*put_prev_ctx)(struct xsched_entity *xse);

	/* Check context preemption. */
	bool (*check_preempt)(struct xsched_entity *xse);

	/* Select jobs from XSE to submit on XCU */
	size_t (*select_work)(struct xsched_cu *xcu, struct xsched_entity *xse);
};

static inline void xsched_init_vsm(struct vstream_metadata *vsm,
				struct vstream_info *vs, vstream_args_t *arg)
{
	vsm->sq_id = arg->sq_id;
	vsm->sqe_num = arg->vk_args.sqe_num;
	vsm->timeout = arg->vk_args.timeout;
	memcpy(vsm->sqe, arg->vk_args.sqe, XCU_SQE_SIZE_MAX);
	vsm->parent = vs;
	INIT_LIST_HEAD(&vsm->node);
}

int xsched_xcu_init(struct xsched_cu *xcu, struct xcu_group *group, int xcu_id);
int xsched_schedule(void *input_xcu);
int xsched_init_entity(struct xsched_context *ctx, struct vstream_info *vs);
int ctx_bind_to_xcu(vstream_info_t *vstream_info, struct xsched_context *ctx);
int xsched_vsm_add_tail(struct vstream_info *vs, vstream_args_t *arg);
struct vstream_metadata *xsched_vsm_fetch_first(struct vstream_info *vs);
int xsched_rt_prio_set(pid_t tgid, unsigned int prio);
void enqueue_ctx(struct xsched_entity *xse, struct xsched_cu *xcu);
void dequeue_ctx(struct xsched_entity *xse, struct xsched_cu *xcu);
int delete_ctx(struct xsched_context *ctx);
#endif /* !__LINUX_XSCHED_H__ */
