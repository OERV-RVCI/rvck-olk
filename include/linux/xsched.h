/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_XSCHED_H__
#define __LINUX_XSCHED_H__

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
	struct task_struct *worker;
	struct xcu_group *group;
	struct mutex xcu_lock;
	wait_queue_head_t wq_xcu_idle;
};

int xsched_xcu_init(struct xsched_cu *xcu, struct xcu_group *group, int xcu_id);
int xsched_schedule(void *input_xcu);
#endif /* !__LINUX_XSCHED_H__ */
