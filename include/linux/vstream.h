/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VSTREAM_H
#define _LINUX_VSTREAM_H

#include <uapi/linux/xcu_vstream.h>

typedef int vstream_manage_t(struct vstream_args *arg);

typedef struct vstream_info {
	uint32_t user_stream_id;
	uint32_t id;
	uint32_t vcq_id;
	uint32_t logic_vcq_id;
	uint32_t dev_id;
	uint32_t channel_id;
	uint32_t fd;
	uint32_t task_type;
	int tgid;
	int sqcq_type;

	void *drv_ctx;

	int inode_fd;

	/* Pointer to corresponding context. */
	struct xsched_context *ctx;

	/* List node in context's vstream list. */
	struct list_head ctx_node;

	/* Pointer to an CU object on which this
	 * vstream is currently being processed.
	 * NULL if vstream is not being processed.
	 */
	struct xsched_cu *xcu;

	/* List node in an CU list of vstreams that
	 * are currently being processed by this specific CU.
	 */
	struct list_head xcu_node;

	/* Private vstream data. */
	void *data;

	spinlock_t stream_lock;

	uint32_t kicks_count;

	/* List of metadata a.k.a. all recorded unprocesed
	 * kicks for this exact vstream.
	 */
	struct list_head metadata_list;
} vstream_info_t;

int vstream_alloc(struct vstream_args *arg);
int vstream_free(struct vstream_args *arg);
int vstream_kick(struct vstream_args *arg);

#endif /* _LINUX_VSTREAM_H */
