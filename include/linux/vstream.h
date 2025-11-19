/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VSTREAM_H
#define _LINUX_VSTREAM_H

#include <uapi/linux/xcu_vstream.h>

typedef int vstream_manage_t(struct vstream_args *arg);

int vstream_alloc(struct vstream_args *arg);
int vstream_free(struct vstream_args *arg);
int vstream_kick(struct vstream_args *arg);

#endif /* _LINUX_VSTREAM_H */
