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

#endif /* !__LINUX_XSCHED_H__ */
