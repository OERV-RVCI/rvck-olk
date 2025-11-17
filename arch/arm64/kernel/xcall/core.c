// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Huawei Limited.
 */

#define pr_fmt(fmt)	"xcall: " fmt

#include <linux/slab.h>
#include <linux/xcall.h>

#include <asm/xcall.h>

static DEFINE_SPINLOCK(xcall_list_lock);
static LIST_HEAD(xcalls_list);
static DEFINE_SPINLOCK(prog_list_lock);
static LIST_HEAD(progs_list);

/*
 * Travel the list of all registered xcall_prog during module installation
 * to find the xcall_prog.
 */
static struct xcall_prog *get_xcall_prog(const char *module)
{
	struct xcall_prog *p;

	list_for_each_entry(p, &progs_list, list) {
		if (!strcmp(module, p->name))
			return p;
	}
	return NULL;
}

static struct xcall_prog *get_xcall_prog_locked(const char *module)
{
	struct xcall_prog *ret;

	spin_lock(&prog_list_lock);
	ret = get_xcall_prog(module);
	spin_unlock(&prog_list_lock);

	return ret;
}

static struct xcall *get_xcall(struct xcall *xcall)
{
	refcount_inc(&xcall->ref);
	return xcall;
}

static void put_xcall(struct xcall *xcall)
{
	if (!refcount_dec_and_test(&xcall->ref))
		return;

	kfree(xcall->name);
	if (xcall->program)
		module_put(xcall->program->owner);

	kfree(xcall);
}

static struct xcall *find_xcall(const char *name, struct inode *binary)
{
	struct xcall *xcall;

	list_for_each_entry(xcall, &xcalls_list, list) {
		if ((name && !strcmp(name, xcall->name)) ||
		    (binary && xcall->binary == binary))
			return get_xcall(xcall);
	}
	return NULL;
}

static struct xcall *insert_xcall_locked(struct xcall *xcall)
{
	struct xcall *ret = NULL;

	spin_lock(&xcall_list_lock);
	ret = find_xcall(xcall->name, xcall->binary);
	if (!ret)
		list_add(&xcall->list, &xcalls_list);
	else
		put_xcall(ret);
	spin_unlock(&xcall_list_lock);
	return ret;
}

static void delete_xcall(struct xcall *xcall)
{
	spin_lock(&xcall_list_lock);
	list_del(&xcall->list);
	spin_unlock(&xcall_list_lock);

	put_xcall(xcall);
}

/* Init xcall with a given inode */
static int init_xcall(struct xcall *xcall, struct xcall_comm *comm)
{
	struct xcall_prog *program = get_xcall_prog_locked(comm->module);

	if (!program || !try_module_get(program->owner))
		return -EINVAL;

	xcall->binary = d_real_inode(comm->binary_path.dentry);
	xcall->program = program;
	refcount_set(&xcall->ref, 1);
	INIT_LIST_HEAD(&xcall->list);

	return 0;
}

int xcall_attach(struct xcall_comm *comm)
{
	struct xcall *xcall;
	int ret;

	xcall = kzalloc(sizeof(struct xcall), GFP_KERNEL);
	if (!xcall)
		return -ENOMEM;

	ret = init_xcall(xcall, comm);
	if (ret) {
		kfree(xcall);
		return ret;
	}

	xcall->name = kstrdup(comm->name, GFP_KERNEL);
	if (!xcall->name) {
		delete_xcall(xcall);
		return -ENOMEM;
	}

	if (insert_xcall_locked(xcall)) {
		delete_xcall(xcall);
		return -EINVAL;
	}

	return 0;
}

int xcall_detach(struct xcall_comm *comm)
{
	struct xcall *xcall;

	spin_lock(&xcall_list_lock);
	xcall = find_xcall(comm->name, NULL);
	if (!xcall) {
		spin_unlock(&xcall_list_lock);
		return -EINVAL;
	}

	put_xcall(xcall);
	list_del(&xcall->list);
	put_xcall(xcall);
	spin_unlock(&xcall_list_lock);
	return 0;
}

static int check_prog(struct xcall_prog *prog)
{
	int prog_len = strnlen(prog->name, PROG_NAME_LEN);
	struct xcall_prog_object *obj = prog->objs;
	DECLARE_BITMAP(hijack_map, NR_syscalls);

	if (!prog->owner)
		return -EINVAL;

	if (prog_len <= 0 || prog_len >= PROG_NAME_LEN)
		return -EINVAL;

	prog->nr_scno = 0;
	bitmap_zero(hijack_map, NR_syscalls);
	while (prog->nr_scno < MAX_NR_SCNO && obj && obj->func) {
		if (obj->scno >= __NR_syscalls)
			return -EINVAL;

		if (test_and_set_bit(obj->scno, hijack_map))
			return -EINVAL;

		prog->nr_scno++;
		obj++;
	}

	if (!prog->nr_scno)
		return -EINVAL;

	return 0;
}

int xcall_prog_register(struct xcall_prog *prog)
{
	if (!static_key_enabled(&xcall_enable))
		return -EACCES;

	if (check_prog(prog))
		return -EINVAL;

	spin_lock(&prog_list_lock);
	if (get_xcall_prog(prog->name)) {
		spin_unlock(&prog_list_lock);
		return -EBUSY;
	}

	INIT_LIST_HEAD(&prog->list);
	list_add(&prog->list, &progs_list);
	spin_unlock(&prog_list_lock);
	return 0;
}
EXPORT_SYMBOL(xcall_prog_register);

void xcall_prog_unregister(struct xcall_prog *prog)
{
	if (!static_key_enabled(&xcall_enable))
		return;

	if (check_prog(prog))
		return;

	spin_lock(&prog_list_lock);
	if (get_xcall_prog(prog->name))
		list_del(&prog->list);
	spin_unlock(&prog_list_lock);
}
EXPORT_SYMBOL(xcall_prog_unregister);
