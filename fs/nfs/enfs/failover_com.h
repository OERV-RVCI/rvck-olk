/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */

#ifndef FAILOVER_COMMON_H
#define FAILOVER_COMMON_H

static inline bool failover_is_enfs_clnt(struct rpc_clnt *clnt)
{
	struct rpc_clnt *next = clnt->cl_parent;
	struct rpc_clnt_reserve *clnt_reserve;

	while (next) {
		if (next == next->cl_parent)
			break;
		next = next->cl_parent;
	}
	if (next != NULL) {
		clnt_reserve = (struct rpc_clnt_reserve *)next;
		return clnt_reserve->cl_enfs == 1 ? true : false;
	}
	clnt_reserve = (struct rpc_clnt_reserve *)clnt;
	return clnt_reserve->cl_enfs == 1 ? true : false;
}

#endif // FAILOVER_COMMON_H
