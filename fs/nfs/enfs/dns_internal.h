/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#ifndef _DNS_INTERNAL_H_
#define _DNS_INTERNAL_H_

#include <linux/compiler.h>
#include <linux/kernel.h>
#include <linux/sched.h>

#include "enfs_log.h"

struct multipath_mount_options;

/*
 * Layout of key payload words.
 */
enum {
	enfs_dns_key_data,
	enfs_dns_key_error,
};

/*
 * dns_key.c
 */
extern const struct cred *enfs_dns_resolver_cache;

/*
 * debug tracing
 */
extern unsigned int enfs_dns_resolver_debug;

#define kdebug(FMT, ...)                                                       \
	do {                                                                   \
		if (unlikely(enfs_dns_resolver_debug))                         \
			pr_debug("ENFS: [%-6.6s] " FMT "\n", current->comm, \
			       ##__VA_ARGS__);                                 \
	} while (0)

#define kenter(FMT, ...) kdebug("==> %s(" FMT ")", __func__, ##__VA_ARGS__)
#define kleave(FMT, ...) kdebug("<== %s()" FMT "", __func__, ##__VA_ARGS__)

void enfs_add_domain_name(struct multipath_mount_options *opt);
void enfs_debug_print_name_list(void);

int enfs_dns_init(void);
void enfs_dns_exit(void);

#endif // _DNS_INTERNAL_H_
