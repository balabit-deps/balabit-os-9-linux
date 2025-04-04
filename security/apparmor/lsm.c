// SPDX-License-Identifier: GPL-2.0-only
/*
 * AppArmor security module
 *
 * This file contains AppArmor LSM hooks.
 *
 * Copyright (C) 1998-2008 Novell/SUSE
 * Copyright 2009-2010 Canonical Ltd.
 */

#include <linux/lsm_hooks.h>
#include <linux/moduleparam.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/ptrace.h>
#include <linux/ctype.h>
#include <linux/sysctl.h>
#include <linux/audit.h>
#include <linux/nsproxy.h>
#include <linux/ipc_namespace.h>
#include <linux/user_namespace.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6.h>
#include <linux/zlib.h>
#include <net/sock.h>
#include <uapi/linux/mount.h>

#include "include/af_unix.h"
#include "include/apparmor.h"
#include "include/apparmorfs.h"
#include "include/audit.h"
#include "include/capability.h"
#include "include/cred.h"
#include "include/file.h"
#include "include/inode.h"
#include "include/ipc.h"
#include "include/net.h"
#include "include/path.h"
#include "include/label.h"
#include "include/policy.h"
#include "include/policy_ns.h"
#include "include/procattr.h"
#include "include/mount.h"
#include "include/secid.h"

/* Flag indicating whether initialization completed */
int apparmor_initialized;

union aa_buffer {
	struct list_head list;
	char buffer[1];
};

#define RESERVE_COUNT 2
static int reserve_count = RESERVE_COUNT;
static int buffer_count;

static LIST_HEAD(aa_global_buffers);
static DEFINE_SPINLOCK(aa_buffers_lock);

static bool is_mqueue_dentry(struct dentry *dentry)
{
	return dentry && is_mqueue_inode(d_backing_inode(dentry));
}

/*
 * LSM hook functions
 */

/*
 * put the associated labels
 */
static void apparmor_cred_free(struct cred *cred)
{
	aa_put_label(cred_label(cred));
	set_cred_label(cred, NULL);
}

/*
 * allocate the apparmor part of blank credentials
 */
static int apparmor_cred_alloc_blank(struct cred *cred, gfp_t gfp)
{
	set_cred_label(cred, NULL);
	return 0;
}

/*
 * prepare new cred label for modification by prepare_cred block
 */
static int apparmor_cred_prepare(struct cred *new, const struct cred *old,
				 gfp_t gfp)
{
	set_cred_label(new, aa_get_newest_label(cred_label(old)));
	return 0;
}

/*
 * transfer the apparmor data to a blank set of creds
 */
static void apparmor_cred_transfer(struct cred *new, const struct cred *old)
{
	set_cred_label(new, aa_get_newest_label(cred_label(old)));
}

static void apparmor_task_free(struct task_struct *task)
{

	aa_free_task_ctx(task_ctx(task));
}

static int apparmor_task_alloc(struct task_struct *task,
			       unsigned long clone_flags)
{
	struct aa_task_ctx *new = task_ctx(task);

	aa_dup_task_ctx(new, task_ctx(current));

	return 0;
}

static int apparmor_ptrace_access_check(struct task_struct *child,
					unsigned int mode)
{
	struct aa_label *tracer, *tracee;
	int error;

	tracer = __begin_current_label_crit_section();
	tracee = aa_get_task_label(child);
	error = aa_may_ptrace(tracer, tracee,
			(mode & PTRACE_MODE_READ) ? AA_PTRACE_READ
						  : AA_PTRACE_TRACE);
	aa_put_label(tracee);
	__end_current_label_crit_section(tracer);

	return error;
}

static int apparmor_ptrace_traceme(struct task_struct *parent)
{
	struct aa_label *tracer, *tracee;
	int error;

	tracee = __begin_current_label_crit_section();
	tracer = aa_get_task_label(parent);
	error = aa_may_ptrace(tracer, tracee, AA_PTRACE_TRACE);
	aa_put_label(tracer);
	__end_current_label_crit_section(tracee);

	return error;
}

/* Derived from security/commoncap.c:cap_capget */
static int apparmor_capget(struct task_struct *target, kernel_cap_t *effective,
			   kernel_cap_t *inheritable, kernel_cap_t *permitted)
{
	struct aa_label *label;
	const struct cred *cred;

	rcu_read_lock();
	cred = __task_cred(target);
	label = aa_get_newest_cred_label(cred);

	/*
	 * cap_capget is stacked ahead of this and will
	 * initialize effective and permitted.
	 */
	if (!unconfined(label)) {
		struct aa_profile *profile;
		struct label_it i;

		label_for_each_confined(i, label, profile) {
			if (COMPLAIN_MODE(profile))
				continue;
			*effective = cap_intersect(*effective,
						   profile->caps.allow);
			*permitted = cap_intersect(*permitted,
						   profile->caps.allow);
		}
	}
	rcu_read_unlock();
	aa_put_label(label);

	return 0;
}

static int apparmor_capable(const struct cred *cred, struct user_namespace *ns,
			    int cap, unsigned int opts)
{
	struct aa_label *label;
	int error = 0;

	label = aa_get_newest_cred_label(cred);
	if (!unconfined(label))
		error = aa_capable(label, cap, opts);
	aa_put_label(label);

	return error;
}

/**
 * common_perm - basic common permission check wrapper fn for paths
 * @op: operation being checked
 * @path: path to check permission of  (NOT NULL)
 * @mask: requested permissions mask
 * @cond: conditional info for the permission request  (NOT NULL)
 *
 * Returns: %0 else error code if error or permission denied
 */
static int common_perm(const char *op, const struct path *path, u32 mask,
		       struct path_cond *cond)
{
	struct aa_label *label;
	int error = 0;

	label = __begin_current_label_crit_section();
	if (!unconfined(label))
		error = aa_path_perm(op, label, path, 0, mask, cond);
	__end_current_label_crit_section(label);

	return error;
}

/**
 * common_perm_cond - common permission wrapper around inode cond
 * @op: operation being checked
 * @path: location to check (NOT NULL)
 * @mask: requested permissions mask
 *
 * Returns: %0 else error code if error or permission denied
 */
static int common_perm_cond(const char *op, const struct path *path, u32 mask)
{
	struct user_namespace *mnt_userns = mnt_user_ns(path->mnt);
	struct path_cond cond = {
		i_uid_into_mnt(mnt_userns, d_backing_inode(path->dentry)),
		d_backing_inode(path->dentry)->i_mode
	};

	if (!path_mediated_fs(path->dentry))
		return 0;

	return common_perm(op, path, mask, &cond);
}

/**
 * common_perm_dir_dentry - common permission wrapper when path is dir, dentry
 * @op: operation being checked
 * @dir: directory of the dentry  (NOT NULL)
 * @dentry: dentry to check  (NOT NULL)
 * @mask: requested permissions mask
 * @cond: conditional info for the permission request  (NOT NULL)
 *
 * Returns: %0 else error code if error or permission denied
 */
static int common_perm_dir_dentry(const char *op, const struct path *dir,
				  struct dentry *dentry, u32 mask,
				  struct path_cond *cond)
{
	struct path path = { .mnt = dir->mnt, .dentry = dentry };

	return common_perm(op, &path, mask, cond);
}

/**
 * common_perm_rm - common permission wrapper for operations doing rm
 * @op: operation being checked
 * @dir: directory that the dentry is in  (NOT NULL)
 * @dentry: dentry being rm'd  (NOT NULL)
 * @mask: requested permission mask
 *
 * Returns: %0 else error code if error or permission denied
 */
static int common_perm_rm(const char *op, const struct path *dir,
			  struct dentry *dentry, u32 mask)
{
	struct inode *inode = d_backing_inode(dentry);
	struct user_namespace *mnt_userns = mnt_user_ns(dir->mnt);
	struct path_cond cond = { };

	if (!inode || !path_mediated_fs(dentry))
		return 0;

	cond.uid = i_uid_into_mnt(mnt_userns, inode);
	cond.mode = inode->i_mode;

	return common_perm_dir_dentry(op, dir, dentry, mask, &cond);
}

/**
 * common_perm_create - common permission wrapper for operations doing create
 * @op: operation being checked
 * @dir: directory that dentry will be created in  (NOT NULL)
 * @dentry: dentry to create   (NOT NULL)
 * @mask: request permission mask
 * @mode: created file mode
 *
 * Returns: %0 else error code if error or permission denied
 */
static int common_perm_create(const char *op, const struct path *dir,
			      struct dentry *dentry, u32 mask, umode_t mode)
{
	struct path_cond cond = { current_fsuid(), mode };

	if (!path_mediated_fs(dir->dentry))
		return 0;

	return common_perm_dir_dentry(op, dir, dentry, mask, &cond);
}

static int apparmor_path_unlink(const struct path *dir, struct dentry *dentry)
{
	return common_perm_rm(OP_UNLINK, dir, dentry, AA_MAY_DELETE);
}

static int apparmor_path_mkdir(const struct path *dir, struct dentry *dentry,
			       umode_t mode)
{
	return common_perm_create(OP_MKDIR, dir, dentry, AA_MAY_CREATE,
				  S_IFDIR);
}

static int apparmor_path_rmdir(const struct path *dir, struct dentry *dentry)
{
	return common_perm_rm(OP_RMDIR, dir, dentry, AA_MAY_DELETE);
}

static int apparmor_path_mknod(const struct path *dir, struct dentry *dentry,
			       umode_t mode, unsigned int dev)
{
	return common_perm_create(OP_MKNOD, dir, dentry, AA_MAY_CREATE, mode);
}

static int apparmor_path_truncate(const struct path *path)
{
	return common_perm_cond(OP_TRUNC, path, MAY_WRITE | AA_MAY_SETATTR);
}

static int apparmor_path_symlink(const struct path *dir, struct dentry *dentry,
				 const char *old_name)
{
	return common_perm_create(OP_SYMLINK, dir, dentry, AA_MAY_CREATE,
				  S_IFLNK);
}

static int apparmor_path_link(struct dentry *old_dentry, const struct path *new_dir,
			      struct dentry *new_dentry)
{
	struct aa_label *label;
	int error = 0;

	if (!path_mediated_fs(old_dentry))
		return 0;

	label = begin_current_label_crit_section();
	if (!unconfined(label))
		error = aa_path_link(label, old_dentry, new_dir, new_dentry);
	end_current_label_crit_section(label);

	return error;
}

static int apparmor_path_rename(const struct path *old_dir, struct dentry *old_dentry,
				const struct path *new_dir, struct dentry *new_dentry)
{
	struct aa_label *label;
	int error = 0;

	if (!path_mediated_fs(old_dentry))
		return 0;

	label = begin_current_label_crit_section();
	if (!unconfined(label)) {
		struct user_namespace *mnt_userns = mnt_user_ns(old_dir->mnt);
		struct path old_path = { .mnt = old_dir->mnt,
					 .dentry = old_dentry };
		struct path new_path = { .mnt = new_dir->mnt,
					 .dentry = new_dentry };
		struct path_cond cond = {
			i_uid_into_mnt(mnt_userns, d_backing_inode(old_dentry)),
			d_backing_inode(old_dentry)->i_mode
		};

		error = aa_path_perm(OP_RENAME_SRC, label, &old_path, 0,
				     MAY_READ | AA_MAY_GETATTR | MAY_WRITE |
				     AA_MAY_SETATTR | AA_MAY_DELETE,
				     &cond);
		if (!error)
			error = aa_path_perm(OP_RENAME_DEST, label, &new_path,
					     0, MAY_WRITE | AA_MAY_SETATTR |
					     AA_MAY_CREATE, &cond);

	}
	end_current_label_crit_section(label);

	return error;
}

static int apparmor_path_chmod(const struct path *path, umode_t mode)
{
	return common_perm_cond(OP_CHMOD, path, AA_MAY_CHMOD);
}

static int apparmor_path_chown(const struct path *path, kuid_t uid, kgid_t gid)
{
	return common_perm_cond(OP_CHOWN, path, AA_MAY_CHOWN);
}

static int common_mqueue_path_perm(const char *op, u32 request,
				   const struct path *path)
{
	struct aa_label *label;
	int error = 0;

	label = begin_current_label_crit_section();
	if (!unconfined(label))
		error = aa_mqueue_perm(OP_UNLINK, label, path, request);

	end_current_label_crit_section(label);

	return error;
}

static int apparmor_inode_getattr(const struct path *path)
{
	if (is_mqueue_dentry(path->dentry))
		/* TODO: fn() for d_parent */
		return common_mqueue_path_perm(OP_UNLINK, AA_MAY_GETATTR, path);

	return common_perm_cond(OP_GETATTR, path, AA_MAY_GETATTR);
}

/* inode security operations */

/* alloced by infrastructure */
static int apparmor_inode_alloc_security(struct inode *inode)
{
	struct aa_inode_sec *isec = apparmor_inode(inode);

	spin_lock_init(&isec->lock);
	isec->inode = inode;
	isec->label = NULL;
	isec->sclass = 0;
	isec->initialized = false;

	return 0;
}

/* freed by infrastructure */
static void apparmor_inode_free_security(struct inode *inode)
{
	struct aa_inode_sec *isec = apparmor_inode(inode);

	if (!isec)
		return;

	aa_put_label(isec->label);
}


/* this is broken, in that we must make it work for ALL xattr fs
 * or it will bail early, so this does not work with LSM stacking
 */
static int apparmor_inode_init_security(struct inode *inode, struct inode *dir,
				       const struct qstr *qstr,
				       const char **name,
				       void **value, size_t *len)
{
	struct aa_inode_sec *isec = apparmor_inode(inode);

	if (is_mqueue_inode(dir)) {
		/* only initialize based on implied label atm */
		isec->label = aa_get_current_label();
		isec->sclass = AA_CLASS_POSIX_MQUEUE;
		isec->initialized = true;
	}

	/* we aren't setting xattrs yet so pretend it isn't supported,
	 * note bug in LSM means other LSMs won't get to init inode either
	 */
	return -EOPNOTSUPP;
}

static int inode_init_with_dentry(struct inode *inode, struct dentry *dentry)
{
	struct aa_inode_sec *isec = apparmor_inode(inode);

	if (isec->initialized)
		return 0;
	spin_lock(&isec->lock);
	/* recheck under lock */
	if (isec->initialized)
		goto unlock;

	if (is_mqueue_sb(inode->i_sb)) {
		/* only initialize based on implied label atm */
		isec->label = aa_get_current_label();
		isec->sclass = AA_CLASS_POSIX_MQUEUE;
		isec->initialized = true;
	}

unlock:
	spin_unlock(&isec->lock);

	return 0;
}

static void apparmor_d_instantiate(struct dentry *dentry, struct inode *inode)
{
	if (inode)
		inode_init_with_dentry(inode, dentry);
}

static int apparmor_inode_create(struct inode *dir, struct dentry *dentry,
				 umode_t mode)
{
	struct aa_label *label;
	int error = 0;

	label = begin_current_label_crit_section();
	if (!unconfined(label)) {
		struct path path = {
			.dentry = dentry,
			.mnt = current->nsproxy->ipc_ns->mq_mnt,
		};
		if (is_mqueue_inode(dir)) {
			error = aa_mqueue_perm(OP_CREATE, label, &path, AA_MAY_CREATE);
		}
	}
	end_current_label_crit_section(label);

	return error;
}

static int common_mqueue_perm(const char *op, u32 request, struct inode *dir, struct dentry *dentry)
{
	/* can't directly determine ipc ns, but know for mqueues dir is mnt_root */
	struct path path = {
		.dentry = dentry,
		.mnt = d_inode(current->nsproxy->ipc_ns->mq_mnt->mnt_root) == dir ? current->nsproxy->ipc_ns->mq_mnt : NULL,
	};

	if (dir != d_inode(current->nsproxy->ipc_ns->mq_mnt->mnt_root))
		pr_warn("apparmor: unlink dir != mnt_root - disconnected");

	return common_mqueue_path_perm(op, request, &path);
}

static int apparmor_inode_unlink(struct inode *dir, struct dentry *dentry)
{
	int error = 0;

	if (is_mqueue_dentry(dentry))
		error = common_mqueue_perm(OP_UNLINK, AA_MAY_DELETE, dir, dentry);

	return error;
}

static int apparmor_inode_setattr(struct dentry *dentry, struct iattr *iattr)
{
	/* TODO: extend to support iattr as a parameter */
	if (is_mqueue_dentry(dentry))
		/* TODO: fn() for d_parent */
		return common_mqueue_perm(OP_UNLINK, AA_MAY_SETATTR,
				  d_backing_inode(dentry->d_parent), dentry);

	return 0;
}

static int apparmor_file_open(struct file *file)
{
	struct aa_file_ctx *fctx = file_ctx(file);
	struct aa_label *label;
	int error = 0;

	if (!path_mediated_fs(file->f_path.dentry))
		return 0;

	/* If in exec, permission is handled by bprm hooks.
	 * Cache permissions granted by the previous exec check, with
	 * implicit read and executable mmap which are required to
	 * actually execute the image.
	 */
	if (current->in_execve) {
		fctx->allow = MAY_EXEC | MAY_READ | AA_EXEC_MMAP;
		return 0;
	}

	label = aa_get_newest_cred_label(file->f_cred);
	if (!unconfined(label)) {
		struct user_namespace *mnt_userns = file_mnt_user_ns(file);
		struct inode *inode = file_inode(file);
		struct path_cond cond = {
			i_uid_into_mnt(mnt_userns, inode),
			inode->i_mode
		};

		if (is_mqueue_inode(file_inode(file)))
			error = aa_mqueue_perm(OP_OPEN, label, &file->f_path,
					       aa_map_file_to_perms(file));
		else
			error = aa_path_perm(OP_OPEN, label, &file->f_path, 0,
					     aa_map_file_to_perms(file), &cond);
		/* todo cache full allowed permissions set and state */
		if (!error)
			fctx->allow = aa_map_file_to_perms(file);
	}
	aa_put_label(label);

	return error;
}

static int apparmor_file_alloc_security(struct file *file)
{
	struct aa_file_ctx *ctx = file_ctx(file);
	struct aa_label *label = begin_current_label_crit_section();

	/* no inode available here */
	spin_lock_init(&ctx->lock);
	rcu_assign_pointer(ctx->label, aa_get_label(label));
	end_current_label_crit_section(label);
	return 0;
}

static void apparmor_file_free_security(struct file *file)
{
	struct aa_file_ctx *ctx = file_ctx(file);

	if (ctx)
		aa_put_label(rcu_access_pointer(ctx->label));
}

static int common_file_perm(const char *op, struct file *file, u32 mask,
			    bool in_atomic)
{
	struct aa_label *label;
	int error = 0;

	/* don't reaudit files closed during inheritance */
	if (file->f_path.dentry == aa_null.dentry)
		return -EACCES;

	label = __begin_current_label_crit_section();
	error = aa_file_perm(op, label, file, mask, in_atomic);
	__end_current_label_crit_section(label);

	return error;
}

static int apparmor_file_receive(struct file *file)
{
	return common_file_perm(OP_FRECEIVE, file, aa_map_file_to_perms(file),
				false);
}

static int apparmor_file_permission(struct file *file, int mask)
{
	return common_file_perm(OP_FPERM, file, mask, false);
}

static int apparmor_file_lock(struct file *file, unsigned int cmd)
{
	u32 mask = AA_MAY_LOCK;

	if (cmd == F_WRLCK)
		mask |= MAY_WRITE;

	return common_file_perm(OP_FLOCK, file, mask, false);
}

static int common_mmap(const char *op, struct file *file, unsigned long prot,
		       unsigned long flags, bool in_atomic)
{
	int mask = 0;

	if (!file || !file_ctx(file))
		return 0;

	if (prot & PROT_READ)
		mask |= MAY_READ;
	/*
	 * Private mappings don't require write perms since they don't
	 * write back to the files
	 */
	if ((prot & PROT_WRITE) && !(flags & MAP_PRIVATE))
		mask |= MAY_WRITE;
	if (prot & PROT_EXEC)
		mask |= AA_EXEC_MMAP;

	return common_file_perm(op, file, mask, in_atomic);
}

static int apparmor_mmap_file(struct file *file, unsigned long reqprot,
			      unsigned long prot, unsigned long flags)
{
	return common_mmap(OP_FMMAP, file, prot, flags, GFP_ATOMIC);
}

static int apparmor_file_mprotect(struct vm_area_struct *vma,
				  unsigned long reqprot, unsigned long prot)
{
	return common_mmap(OP_FMPROT, vma->vm_file, prot,
			   !(vma->vm_flags & VM_SHARED) ? MAP_PRIVATE : 0,
			   false);
}

static int apparmor_sb_mount(const char *dev_name, const struct path *path,
			     const char *type, unsigned long flags, void *data)
{
	struct aa_label *label;
	int error = 0;

	/* Discard magic */
	if ((flags & MS_MGC_MSK) == MS_MGC_VAL)
		flags &= ~MS_MGC_MSK;

	flags &= ~AA_MS_IGNORE_MASK;

	label = __begin_current_label_crit_section();
	if (!unconfined(label)) {
		if (flags & MS_REMOUNT)
			error = aa_remount(label, path, flags, data);
		else if (flags & MS_BIND)
			error = aa_bind_mount(label, path, dev_name, flags);
		else if (flags & (MS_SHARED | MS_PRIVATE | MS_SLAVE |
				  MS_UNBINDABLE))
			error = aa_mount_change_type(label, path, flags);
		else if (flags & MS_MOVE)
			error = aa_move_mount(label, path, dev_name);
		else
			error = aa_new_mount(label, dev_name, path, type,
					     flags, data);
	}
	__end_current_label_crit_section(label);

	return error;
}

static int apparmor_sb_umount(struct vfsmount *mnt, int flags)
{
	struct aa_label *label;
	int error = 0;

	label = __begin_current_label_crit_section();
	if (!unconfined(label))
		error = aa_umount(label, mnt, flags);
	__end_current_label_crit_section(label);

	return error;
}

static int apparmor_sb_pivotroot(const struct path *old_path,
				 const struct path *new_path)
{
	struct aa_label *label;
	int error = 0;

	label = aa_get_current_label();
	if (!unconfined(label))
		error = aa_pivotroot(label, old_path, new_path);
	aa_put_label(label);

	return error;
}

static int apparmor_getprocattr(struct task_struct *task, char *name,
				char **value)
{
	int error = -ENOENT;
	/* released below */
	const struct cred *cred = get_task_cred(task);
	struct aa_task_ctx *ctx = task_ctx(current);
	struct aa_label *label = NULL;
	bool newline = true;

	if (strcmp(name, "current") == 0)
		label = aa_get_newest_label(cred_label(cred));
	else if (strcmp(name, "prev") == 0  && ctx->previous)
		label = aa_get_newest_label(ctx->previous);
	else if (strcmp(name, "exec") == 0 && ctx->onexec)
		label = aa_get_newest_label(ctx->onexec);
	else if (strcmp(name, "context") == 0) {
		label = aa_get_newest_label(cred_label(cred));
		newline = false;
	} else
		error = -EINVAL;

	if (label)
		error = aa_getprocattr(label, value, newline);

	aa_put_label(label);
	put_cred(cred);

	return error;
}


static int profile_display_lsm(struct aa_profile *profile,
			       struct common_audit_data *sa)
{
	struct aa_perms perms = { };
	unsigned int state;

	state = PROFILE_MEDIATES(profile, AA_CLASS_DISPLAY_LSM);
	if (state) {
		aa_compute_perms(profile->policy.dfa, state, &perms);
		aa_apply_modes_to_perms(profile, &perms);
		aad(sa)->label = &profile->label;

		return aa_check_perms(profile, &perms, AA_MAY_WRITE, sa, NULL);
	}

	return 0;
}

static int apparmor_setprocattr(const char *name, void *value,
				size_t size)
{
	char *command, *largs = NULL, *args = value;
	size_t arg_size;
	int error;
	DEFINE_AUDIT_DATA(sa, LSM_AUDIT_DATA_NONE, OP_SETPROCATTR);

	if (size == 0)
		return -EINVAL;

	/* LSM infrastructure does actual setting of display if allowed */
	if (!strcmp(name, "display")) {
		struct aa_profile *profile;
		struct aa_label *label;

		aad(&sa)->info = "set display lsm";
		label = begin_current_label_crit_section();
		error = fn_for_each_confined(label, profile,
					     profile_display_lsm(profile, &sa));
		end_current_label_crit_section(label);
		return error;
	}

	/* AppArmor requires that the buffer must be null terminated atm */
	if (args[size - 1] != '\0') {
		/* null terminate */
		largs = args = kmalloc(size + 1, GFP_KERNEL);
		if (!args)
			return -ENOMEM;
		memcpy(args, value, size);
		args[size] = '\0';
	}

	error = -EINVAL;
	args = strim(args);
	command = strsep(&args, " ");
	if (!args)
		goto out;
	args = skip_spaces(args);
	if (!*args)
		goto out;

	arg_size = size - (args - (largs ? largs : (char *) value));
	if (strcmp(name, "current") == 0) {
		if (strcmp(command, "changehat") == 0) {
			error = aa_setprocattr_changehat(args, arg_size,
							 AA_CHANGE_NOFLAGS);
		} else if (strcmp(command, "permhat") == 0) {
			error = aa_setprocattr_changehat(args, arg_size,
							 AA_CHANGE_TEST);
		} else if (strcmp(command, "changeprofile") == 0) {
			error = aa_change_profile(args, AA_CHANGE_NOFLAGS);
		} else if (strcmp(command, "permprofile") == 0) {
			error = aa_change_profile(args, AA_CHANGE_TEST);
		} else if (strcmp(command, "stack") == 0) {
			error = aa_change_profile(args, AA_CHANGE_STACK);
		} else
			goto fail;
	} else if (strcmp(name, "exec") == 0) {
		if (strcmp(command, "exec") == 0)
			error = aa_change_profile(args, AA_CHANGE_ONEXEC);
		else if (strcmp(command, "stack") == 0)
			error = aa_change_profile(args, (AA_CHANGE_ONEXEC |
							 AA_CHANGE_STACK));
		else
			goto fail;
	} else
		/* only support the "current" and "exec" process attributes */
		goto fail;

	if (!error)
		error = size;
out:
	kfree(largs);
	return error;

fail:
	aad(&sa)->label = begin_current_label_crit_section();
	aad(&sa)->info = name;
	aad(&sa)->error = error = -EINVAL;
	aa_audit_msg(AUDIT_APPARMOR_DENIED, &sa, NULL);
	end_current_label_crit_section(aad(&sa)->label);
	goto out;
}

/**
 * apparmor_bprm_committing_creds - do task cleanup on committing new creds
 * @bprm: binprm for the exec  (NOT NULL)
 */
static void apparmor_bprm_committing_creds(struct linux_binprm *bprm)
{
	struct aa_label *label = aa_current_raw_label();
	struct aa_label *new_label = cred_label(bprm->cred);

	/* bail out if unconfined or not changing profile */
	if ((new_label->proxy == label->proxy) ||
	    (unconfined(new_label)))
		return;

	aa_inherit_files(bprm->cred, current->files);

	current->pdeath_signal = 0;

	/* reset soft limits and set hard limits for the new label */
	__aa_transition_rlimits(label, new_label);
}

/**
 * apparmor_bprm_committed_cred - do cleanup after new creds committed
 * @bprm: binprm for the exec  (NOT NULL)
 */
static void apparmor_bprm_committed_creds(struct linux_binprm *bprm)
{
	/* clear out temporary/transitional state from the context */
	aa_clear_task_ctx_trans(task_ctx(current));

	return;
}

static void apparmor_task_getsecid(struct task_struct *p, u32 *secid)
{
	struct aa_label *label = aa_get_task_label(p);
	*secid = label->secid;
	aa_put_label(label);
}

static int apparmor_task_setrlimit(struct task_struct *task,
		unsigned int resource, struct rlimit *new_rlim)
{
	struct aa_label *label = __begin_current_label_crit_section();
	int error = 0;

	if (!unconfined(label))
		error = aa_task_setrlimit(label, task, resource, new_rlim);
	__end_current_label_crit_section(label);

	return error;
}

static int apparmor_task_kill(struct task_struct *target, struct kernel_siginfo *info,
			      int sig, const struct cred *cred)
{
	struct aa_label *cl, *tl;
	int error;

	if (cred) {
		/*
		 * Dealing with USB IO specific behavior
		 */
		cl = aa_get_newest_cred_label(cred);
		tl = aa_get_task_label(target);
		error = aa_may_signal(cl, tl, sig);
		aa_put_label(cl);
		aa_put_label(tl);
		return error;
	}

	cl = __begin_current_label_crit_section();
	tl = aa_get_task_label(target);
	error = aa_may_signal(cl, tl, sig);
	aa_put_label(tl);
	__end_current_label_crit_section(cl);

	return error;
}

/**
 * apparmor_sk_free_security - free the sk_security field
 */
static void apparmor_sk_free_security(struct sock *sk)
{
	struct aa_sk_ctx *ctx = aa_sock(sk);

	aa_put_label(ctx->label);
	aa_put_label(ctx->peer);
	path_put(&ctx->path);
}

/**
 * apparmor_clone_security - clone the sk_security field
 */
static void apparmor_sk_clone_security(const struct sock *sk,
				       struct sock *newsk)
{
	struct aa_sk_ctx *ctx = aa_sock(sk);
	struct aa_sk_ctx *new = aa_sock(newsk);

	if (new->label)
		aa_put_label(new->label);
	new->label = aa_get_label(ctx->label);

	if (new->peer)
		aa_put_label(new->peer);
	new->peer = aa_get_label(ctx->peer);
	new->path = ctx->path;
	path_get(&new->path);
}

static struct path *UNIX_FS_CONN_PATH(struct sock *sk, struct sock *newsk)
{
	if (sk->sk_family == PF_UNIX && UNIX_FS(sk))
		return &unix_sk(sk)->path;
	else if (newsk->sk_family == PF_UNIX && UNIX_FS(newsk))
		return &unix_sk(newsk)->path;
	return NULL;
}

/**
 * apparmor_unix_stream_connect - check perms before making unix domain conn
 *
 * peer is locked when this hook is called
 */
static int apparmor_unix_stream_connect(struct sock *sk, struct sock *peer_sk,
					struct sock *newsk)
{
	struct aa_sk_ctx *sk_ctx = aa_sock(sk);
	struct aa_sk_ctx *peer_ctx = aa_sock(peer_sk);
	struct aa_sk_ctx *new_ctx = aa_sock(newsk);
	struct aa_label *label;
	struct path *path;
	int error;

	label = __begin_current_label_crit_section();
	error = aa_unix_peer_perm(label, OP_CONNECT,
				(AA_MAY_CONNECT | AA_MAY_SEND | AA_MAY_RECEIVE),
				  sk, peer_sk, NULL);
	if (!UNIX_FS(peer_sk)) {
		last_error(error,
			aa_unix_peer_perm(peer_ctx->label, OP_CONNECT,
				(AA_MAY_ACCEPT | AA_MAY_SEND | AA_MAY_RECEIVE),
				peer_sk, sk, label));
	}
	__end_current_label_crit_section(label);

	if (error)
		return error;

	/* label newsk if it wasn't labeled in post_create. Normally this
	 * would be done in sock_graft, but because we are directly looking
	 * at the peer_sk to obtain peer_labeling for unix socks this
	 * does not work
	 */
	if (!new_ctx->label)
		new_ctx->label = aa_get_label(peer_ctx->label);

	/* Cross reference the peer labels for SO_PEERSEC */
	if (new_ctx->peer)
		aa_put_label(new_ctx->peer);

	if (sk_ctx->peer)
		aa_put_label(sk_ctx->peer);

	new_ctx->peer = aa_get_label(sk_ctx->label);
	sk_ctx->peer = aa_get_label(peer_ctx->label);

	path = UNIX_FS_CONN_PATH(sk, peer_sk);
	if (path) {
		new_ctx->path = *path;
		sk_ctx->path = *path;
		path_get(path);
		path_get(path);
	}
	return 0;
}

/**
 * apparmor_unix_may_send - check perms before conn or sending unix dgrams
 *
 * other is locked when this hook is called
 *
 * dgram connect calls may_send, peer setup but path not copied?????
 */
static int apparmor_unix_may_send(struct socket *sock, struct socket *peer)
{
	struct aa_sk_ctx *peer_ctx = aa_sock(peer->sk);
	struct aa_label *label;
	int error;

	label = __begin_current_label_crit_section();
	error = xcheck(aa_unix_peer_perm(label, OP_SENDMSG, AA_MAY_SEND,
					 sock->sk, peer->sk, NULL),
		       aa_unix_peer_perm(peer_ctx->label, OP_SENDMSG,
					 AA_MAY_RECEIVE,
					 peer->sk, sock->sk, label));
	__end_current_label_crit_section(label);

	return error;
}

/**
 * apparmor_socket_create - check perms before creating a new socket
 */
static int apparmor_socket_create(int family, int type, int protocol, int kern)
{
	struct aa_label *label;
	int error = 0;

	AA_BUG(in_interrupt());

	label = begin_current_label_crit_section();
	if (!(kern || unconfined(label)))
		error = af_select(family,
				  create_perm(label, family, type, protocol),
				  aa_af_perm(label, OP_CREATE, AA_MAY_CREATE,
					     family, type, protocol));
	end_current_label_crit_section(label);

	return error;
}

/**
 * apparmor_socket_post_create - setup the per-socket security struct
 *
 * Note:
 * -   kernel sockets currently labeled unconfined but we may want to
 *     move to a special kernel label
 * -   socket may not have sk here if created with sock_create_lite or
 *     sock_alloc. These should be accept cases which will be handled in
 *     sock_graft.
 */
static int apparmor_socket_post_create(struct socket *sock, int family,
				       int type, int protocol, int kern)
{
	struct aa_label *label;

	if (kern) {
		struct aa_ns *ns = aa_get_current_ns();

		label = aa_get_label(ns_unconfined(ns));
		aa_put_ns(ns);
	} else
		label = aa_get_current_label();

	if (sock->sk) {
		struct aa_sk_ctx *ctx = aa_sock(sock->sk);

		aa_put_label(ctx->label);
		ctx->label = aa_get_label(label);
	}
	aa_put_label(label);

	return 0;
}

/**
 * apparmor_socket_bind - check perms before bind addr to socket
 */
static int apparmor_socket_bind(struct socket *sock,
				struct sockaddr *address, int addrlen)
{
	AA_BUG(!sock);
	AA_BUG(!sock->sk);
	AA_BUG(!address);
	AA_BUG(in_interrupt());

	return af_select(sock->sk->sk_family,
			 bind_perm(sock, address, addrlen),
			 aa_sk_perm(OP_BIND, AA_MAY_BIND, sock->sk));
}

/**
 * apparmor_socket_connect - check perms before connecting @sock to @address
 */
static int apparmor_socket_connect(struct socket *sock,
				   struct sockaddr *address, int addrlen)
{
	AA_BUG(!sock);
	AA_BUG(!sock->sk);
	AA_BUG(!address);
	AA_BUG(in_interrupt());

	return af_select(sock->sk->sk_family,
			 connect_perm(sock, address, addrlen),
			 aa_sk_perm(OP_CONNECT, AA_MAY_CONNECT, sock->sk));
}

/**
 * apparmor_socket_list - check perms before allowing listen
 */
static int apparmor_socket_listen(struct socket *sock, int backlog)
{
	AA_BUG(!sock);
	AA_BUG(!sock->sk);
	AA_BUG(in_interrupt());

	return af_select(sock->sk->sk_family,
			 listen_perm(sock, backlog),
			 aa_sk_perm(OP_LISTEN, AA_MAY_LISTEN, sock->sk));
}

/**
 * apparmor_socket_accept - check perms before accepting a new connection.
 *
 * Note: while @newsock is created and has some information, the accept
 *       has not been done.
 */
static int apparmor_socket_accept(struct socket *sock, struct socket *newsock)
{
	AA_BUG(!sock);
	AA_BUG(!sock->sk);
	AA_BUG(!newsock);
	AA_BUG(in_interrupt());

	return af_select(sock->sk->sk_family,
			 accept_perm(sock, newsock),
			 aa_sk_perm(OP_ACCEPT, AA_MAY_ACCEPT, sock->sk));
}

static int aa_sock_msg_perm(const char *op, u32 request, struct socket *sock,
			    struct msghdr *msg, int size)
{
	AA_BUG(!sock);
	AA_BUG(!sock->sk);
	AA_BUG(!msg);
	AA_BUG(in_interrupt());

	return af_select(sock->sk->sk_family,
			 msg_perm(op, request, sock, msg, size),
			 aa_sk_perm(op, request, sock->sk));
}

/**
 * apparmor_socket_sendmsg - check perms before sending msg to another socket
 */
static int apparmor_socket_sendmsg(struct socket *sock,
				   struct msghdr *msg, int size)
{
	return aa_sock_msg_perm(OP_SENDMSG, AA_MAY_SEND, sock, msg, size);
}

/**
 * apparmor_socket_recvmsg - check perms before receiving a message
 */
static int apparmor_socket_recvmsg(struct socket *sock,
				   struct msghdr *msg, int size, int flags)
{
	return aa_sock_msg_perm(OP_RECVMSG, AA_MAY_RECEIVE, sock, msg, size);
}

/* revaliation, get/set attr, shutdown */
static int aa_sock_perm(const char *op, u32 request, struct socket *sock)
{
	AA_BUG(!sock);
	AA_BUG(!sock->sk);
	AA_BUG(in_interrupt());

	return af_select(sock->sk->sk_family,
			 sock_perm(op, request, sock),
			 aa_sk_perm(op, request, sock->sk));
}

/**
 * apparmor_socket_getsockname - check perms before getting the local address
 */
static int apparmor_socket_getsockname(struct socket *sock)
{
	return aa_sock_perm(OP_GETSOCKNAME, AA_MAY_GETATTR, sock);
}

/**
 * apparmor_socket_getpeername - check perms before getting remote address
 */
static int apparmor_socket_getpeername(struct socket *sock)
{
	return aa_sock_perm(OP_GETPEERNAME, AA_MAY_GETATTR, sock);
}

/* revaliation, get/set attr, opt */
static int aa_sock_opt_perm(const char *op, u32 request, struct socket *sock,
			    int level, int optname)
{
	AA_BUG(!sock);
	AA_BUG(!sock->sk);
	AA_BUG(in_interrupt());

	return af_select(sock->sk->sk_family,
			 opt_perm(op, request, sock, level, optname),
			 aa_sk_perm(op, request, sock->sk));
}

/**
 * apparmor_getsockopt - check perms before getting socket options
 */
static int apparmor_socket_getsockopt(struct socket *sock, int level,
				      int optname)
{
	return aa_sock_opt_perm(OP_GETSOCKOPT, AA_MAY_GETOPT, sock,
				level, optname);
}

/**
 * apparmor_setsockopt - check perms before setting socket options
 */
static int apparmor_socket_setsockopt(struct socket *sock, int level,
				      int optname)
{
	return aa_sock_opt_perm(OP_SETSOCKOPT, AA_MAY_SETOPT, sock,
				level, optname);
}

/**
 * apparmor_socket_shutdown - check perms before shutting down @sock conn
 */
static int apparmor_socket_shutdown(struct socket *sock, int how)
{
	return aa_sock_perm(OP_SHUTDOWN, AA_MAY_SHUTDOWN, sock);
}

#ifdef CONFIG_NETWORK_SECMARK
/**
 * apparmor_socket_sock_recv_skb - check perms before associating skb to sk
 *
 * Note: can not sleep may be called with locks held
 *
 * dont want protocol specific in __skb_recv_datagram()
 * to deny an incoming connection  socket_sock_rcv_skb()
 */
static int apparmor_socket_sock_rcv_skb(struct sock *sk, struct sk_buff *skb)
{
	struct aa_sk_ctx *ctx = aa_sock(sk);

	if (!skb->secmark)
		return 0;

	/*
	 * If reach here before socket_post_create hook is called, in which
	 * case label is null, drop the packet.
	 */
	if (!ctx->label)
		return -EACCES;

	return apparmor_secmark_check(ctx->label, OP_RECVMSG, AA_MAY_RECEIVE,
				      skb->secmark, sk);
}
#endif


static struct aa_label *sk_peer_label(struct sock *sk)
{
	struct sock *peer_sk;
	struct aa_sk_ctx *ctx = aa_sock(sk);
	struct aa_label *label = ERR_PTR(-ENOPROTOOPT);

	if (ctx->peer)
		return aa_get_label(ctx->peer);

	if (sk->sk_family != PF_UNIX)
		return ERR_PTR(-ENOPROTOOPT);

	/* check for sockpair peering which does not go through
	 * security_unix_stream_connect
	 */
	peer_sk = unix_peer_get(sk);
	if (peer_sk) {
		ctx = aa_sock(peer_sk);
		if (ctx->label)
			label = aa_get_label(ctx->label);
		sock_put(peer_sk);
	}

	return label;
}

/**
 * apparmor_socket_getpeersec_stream - get security context of peer
 *
 * Note: for tcp only valid if using ipsec or cipso on lan
 */
static int apparmor_socket_getpeersec_stream(struct socket *sock,
					     char __user *optval,
					     int __user *optlen,
					     unsigned int len)
{
	char *name;
	int slen, error = 0;
	struct aa_label *label;
	struct aa_label *peer;

	label = begin_current_label_crit_section();
	peer = sk_peer_label(sock->sk);
	if (IS_ERR(peer)) {
		error = PTR_ERR(peer);
		goto done;
	}
	slen = aa_label_asxprint(&name, labels_ns(label), peer,
				 FLAG_SHOW_MODE | FLAG_VIEW_SUBNS |
				 FLAG_HIDDEN_UNCONFINED, GFP_KERNEL);
	/* don't include terminating \0 in slen, it breaks some apps */
	if (slen < 0) {
		error = -ENOMEM;
	} else {
		if (slen > len) {
			error = -ERANGE;
		} else if (copy_to_user(optval, name, slen)) {
			error = -EFAULT;
			goto out;
		}
		if (put_user(slen, optlen))
			error = -EFAULT;
out:
		kfree(name);

	}

	aa_put_label(peer);
done:
	end_current_label_crit_section(label);

	return error;
}

/**
 * apparmor_sock_graft - Initialize newly created socket
 * @sk: child sock
 * @parent: parent socket
 *
 * Note: could set off of SOCK_CTX(parent) but need to track inode and we can
 *       just set sk security information off of current creating process label
 *       Labeling of sk for accept case - probably should be sock based
 *       instead of task, because of the case where an implicitly labeled
 *       socket is shared by different tasks.
 */
static void apparmor_sock_graft(struct sock *sk, struct socket *parent)
{
	struct aa_sk_ctx *ctx = aa_sock(sk);

	if (!ctx->label)
		ctx->label = aa_get_current_label();
}

#ifdef CONFIG_NETWORK_SECMARK
static int apparmor_inet_conn_request(const struct sock *sk, struct sk_buff *skb,
				      struct request_sock *req)
{
	struct aa_sk_ctx *ctx = aa_sock(sk);

	if (!skb->secmark)
		return 0;

	return apparmor_secmark_check(ctx->label, OP_CONNECT, AA_MAY_CONNECT,
				      skb->secmark, sk);
}
#endif

/*
 * The cred blob is a pointer to, not an instance of, an aa_label.
 */
struct lsm_blob_sizes apparmor_blob_sizes __lsm_ro_after_init = {
	.lbs_cred = sizeof(struct aa_label *),
	.lbs_file = sizeof(struct aa_file_ctx),
	.lbs_inode = sizeof(struct aa_inode_sec),
	.lbs_task = sizeof(struct aa_task_ctx),
	.lbs_sock = sizeof(struct aa_sk_ctx),
	.lbs_ipc = sizeof(struct aa_ipc_sec),
	.lbs_msg_msg = sizeof(struct aa_msg_sec),
	.lbs_superblock = sizeof(struct aa_superblock_sec),
};

static struct lsm_id apparmor_lsmid __lsm_ro_after_init = {
	.lsm  = "apparmor",
	.slot = LSMBLOB_NEEDED
};

static struct security_hook_list apparmor_hooks[] __lsm_ro_after_init = {
	LSM_HOOK_INIT(ptrace_access_check, apparmor_ptrace_access_check),
	LSM_HOOK_INIT(ptrace_traceme, apparmor_ptrace_traceme),
	LSM_HOOK_INIT(capget, apparmor_capget),
	LSM_HOOK_INIT(capable, apparmor_capable),

	LSM_HOOK_INIT(sb_mount, apparmor_sb_mount),
	LSM_HOOK_INIT(sb_umount, apparmor_sb_umount),
	LSM_HOOK_INIT(sb_pivotroot, apparmor_sb_pivotroot),

	LSM_HOOK_INIT(path_link, apparmor_path_link),
	LSM_HOOK_INIT(path_unlink, apparmor_path_unlink),
	LSM_HOOK_INIT(path_symlink, apparmor_path_symlink),
	LSM_HOOK_INIT(path_mkdir, apparmor_path_mkdir),
	LSM_HOOK_INIT(path_rmdir, apparmor_path_rmdir),
	LSM_HOOK_INIT(path_mknod, apparmor_path_mknod),
	LSM_HOOK_INIT(path_rename, apparmor_path_rename),
	LSM_HOOK_INIT(path_chmod, apparmor_path_chmod),
	LSM_HOOK_INIT(path_chown, apparmor_path_chown),
	LSM_HOOK_INIT(path_truncate, apparmor_path_truncate),
	LSM_HOOK_INIT(inode_getattr, apparmor_inode_getattr),

	LSM_HOOK_INIT(inode_alloc_security, apparmor_inode_alloc_security),
	LSM_HOOK_INIT(inode_free_security, apparmor_inode_free_security),
	LSM_HOOK_INIT(inode_init_security, apparmor_inode_init_security),
	LSM_HOOK_INIT(d_instantiate, apparmor_d_instantiate),

	LSM_HOOK_INIT(inode_create, apparmor_inode_create),
	LSM_HOOK_INIT(inode_unlink, apparmor_inode_unlink),
	LSM_HOOK_INIT(inode_setattr, apparmor_inode_setattr),
	LSM_HOOK_INIT(inode_getattr, apparmor_inode_getattr),

	LSM_HOOK_INIT(file_open, apparmor_file_open),
	LSM_HOOK_INIT(file_receive, apparmor_file_receive),
	LSM_HOOK_INIT(file_permission, apparmor_file_permission),
	LSM_HOOK_INIT(file_alloc_security, apparmor_file_alloc_security),
	LSM_HOOK_INIT(file_free_security, apparmor_file_free_security),
	LSM_HOOK_INIT(mmap_file, apparmor_mmap_file),
	LSM_HOOK_INIT(file_mprotect, apparmor_file_mprotect),
	LSM_HOOK_INIT(file_lock, apparmor_file_lock),

	LSM_HOOK_INIT(getprocattr, apparmor_getprocattr),
	LSM_HOOK_INIT(setprocattr, apparmor_setprocattr),

	LSM_HOOK_INIT(sk_free_security, apparmor_sk_free_security),
	LSM_HOOK_INIT(sk_clone_security, apparmor_sk_clone_security),

	LSM_HOOK_INIT(unix_stream_connect, apparmor_unix_stream_connect),
	LSM_HOOK_INIT(unix_may_send, apparmor_unix_may_send),

	LSM_HOOK_INIT(socket_create, apparmor_socket_create),
	LSM_HOOK_INIT(socket_post_create, apparmor_socket_post_create),
	LSM_HOOK_INIT(socket_bind, apparmor_socket_bind),
	LSM_HOOK_INIT(socket_connect, apparmor_socket_connect),
	LSM_HOOK_INIT(socket_listen, apparmor_socket_listen),
	LSM_HOOK_INIT(socket_accept, apparmor_socket_accept),
	LSM_HOOK_INIT(socket_sendmsg, apparmor_socket_sendmsg),
	LSM_HOOK_INIT(socket_recvmsg, apparmor_socket_recvmsg),
	LSM_HOOK_INIT(socket_getsockname, apparmor_socket_getsockname),
	LSM_HOOK_INIT(socket_getpeername, apparmor_socket_getpeername),
	LSM_HOOK_INIT(socket_getsockopt, apparmor_socket_getsockopt),
	LSM_HOOK_INIT(socket_setsockopt, apparmor_socket_setsockopt),
	LSM_HOOK_INIT(socket_shutdown, apparmor_socket_shutdown),
#ifdef CONFIG_NETWORK_SECMARK
	LSM_HOOK_INIT(socket_sock_rcv_skb, apparmor_socket_sock_rcv_skb),
#endif
	LSM_HOOK_INIT(socket_getpeersec_stream,
		      apparmor_socket_getpeersec_stream),
	LSM_HOOK_INIT(sock_graft, apparmor_sock_graft),
#ifdef CONFIG_NETWORK_SECMARK
	LSM_HOOK_INIT(inet_conn_request, apparmor_inet_conn_request),
#endif

	LSM_HOOK_INIT(cred_alloc_blank, apparmor_cred_alloc_blank),
	LSM_HOOK_INIT(cred_free, apparmor_cred_free),
	LSM_HOOK_INIT(cred_prepare, apparmor_cred_prepare),
	LSM_HOOK_INIT(cred_transfer, apparmor_cred_transfer),

	LSM_HOOK_INIT(bprm_creds_for_exec, apparmor_bprm_creds_for_exec),
	LSM_HOOK_INIT(bprm_committing_creds, apparmor_bprm_committing_creds),
	LSM_HOOK_INIT(bprm_committed_creds, apparmor_bprm_committed_creds),

	LSM_HOOK_INIT(task_free, apparmor_task_free),
	LSM_HOOK_INIT(task_alloc, apparmor_task_alloc),
	LSM_HOOK_INIT(task_getsecid_subj, apparmor_task_getsecid),
	LSM_HOOK_INIT(task_getsecid_obj, apparmor_task_getsecid),
	LSM_HOOK_INIT(task_setrlimit, apparmor_task_setrlimit),
	LSM_HOOK_INIT(task_kill, apparmor_task_kill),

#ifdef CONFIG_AUDIT
	LSM_HOOK_INIT(audit_rule_init, aa_audit_rule_init),
	LSM_HOOK_INIT(audit_rule_known, aa_audit_rule_known),
	LSM_HOOK_INIT(audit_rule_match, aa_audit_rule_match),
	LSM_HOOK_INIT(audit_rule_free, aa_audit_rule_free),
#endif

	LSM_HOOK_INIT(secid_to_secctx, apparmor_secid_to_secctx),
	LSM_HOOK_INIT(secctx_to_secid, apparmor_secctx_to_secid),
	LSM_HOOK_INIT(release_secctx, apparmor_release_secctx),
};

/*
 * AppArmor sysfs module parameters
 */

static int param_set_aabool(const char *val, const struct kernel_param *kp);
static int param_get_aabool(char *buffer, const struct kernel_param *kp);
#define param_check_aabool param_check_bool
static const struct kernel_param_ops param_ops_aabool = {
	.flags = KERNEL_PARAM_OPS_FL_NOARG,
	.set = param_set_aabool,
	.get = param_get_aabool
};

static int param_set_aauint(const char *val, const struct kernel_param *kp);
static int param_get_aauint(char *buffer, const struct kernel_param *kp);
#define param_check_aauint param_check_uint
static const struct kernel_param_ops param_ops_aauint = {
	.set = param_set_aauint,
	.get = param_get_aauint
};

static int param_set_aacompressionlevel(const char *val,
					const struct kernel_param *kp);
static int param_get_aacompressionlevel(char *buffer,
					const struct kernel_param *kp);
#define param_check_aacompressionlevel param_check_int
static const struct kernel_param_ops param_ops_aacompressionlevel = {
	.set = param_set_aacompressionlevel,
	.get = param_get_aacompressionlevel
};

static int param_set_aalockpolicy(const char *val, const struct kernel_param *kp);
static int param_get_aalockpolicy(char *buffer, const struct kernel_param *kp);
#define param_check_aalockpolicy param_check_bool
static const struct kernel_param_ops param_ops_aalockpolicy = {
	.flags = KERNEL_PARAM_OPS_FL_NOARG,
	.set = param_set_aalockpolicy,
	.get = param_get_aalockpolicy
};

static int param_set_audit(const char *val, const struct kernel_param *kp);
static int param_get_audit(char *buffer, const struct kernel_param *kp);

static int param_set_mode(const char *val, const struct kernel_param *kp);
static int param_get_mode(char *buffer, const struct kernel_param *kp);

/* Flag values, also controllable via /sys/module/apparmor/parameters
 * We define special types as we want to do additional mediation.
 */

/* AppArmor global enforcement switch - complain, enforce, kill */
enum profile_mode aa_g_profile_mode = APPARMOR_ENFORCE;
module_param_call(mode, param_set_mode, param_get_mode,
		  &aa_g_profile_mode, S_IRUSR | S_IWUSR);

/* whether policy verification hashing is enabled */
bool aa_g_hash_policy = IS_ENABLED(CONFIG_SECURITY_APPARMOR_HASH_DEFAULT);
#ifdef CONFIG_SECURITY_APPARMOR_HASH
module_param_named(hash_policy, aa_g_hash_policy, aabool, S_IRUSR | S_IWUSR);
#endif

/* policy loaddata compression level */
int aa_g_rawdata_compression_level = Z_DEFAULT_COMPRESSION;
module_param_named(rawdata_compression_level, aa_g_rawdata_compression_level,
		   aacompressionlevel, 0400);

/* Debug mode */
bool aa_g_debug = IS_ENABLED(CONFIG_SECURITY_APPARMOR_DEBUG_MESSAGES);
module_param_named(debug, aa_g_debug, aabool, S_IRUSR | S_IWUSR);

/* Audit mode */
enum audit_mode aa_g_audit;
module_param_call(audit, param_set_audit, param_get_audit,
		  &aa_g_audit, S_IRUSR | S_IWUSR);

/* Determines if audit header is included in audited messages.  This
 * provides more context if the audit daemon is not running
 */
bool aa_g_audit_header = true;
module_param_named(audit_header, aa_g_audit_header, aabool,
		   S_IRUSR | S_IWUSR);

/* lock out loading/removal of policy
 * TODO: add in at boot loading of policy, which is the only way to
 *       load policy, if lock_policy is set
 */
bool aa_g_lock_policy;
module_param_named(lock_policy, aa_g_lock_policy, aalockpolicy,
		   S_IRUSR | S_IWUSR);

/* Syscall logging mode */
bool aa_g_logsyscall;
module_param_named(logsyscall, aa_g_logsyscall, aabool, S_IRUSR | S_IWUSR);

/* Maximum pathname length before accesses will start getting rejected */
unsigned int aa_g_path_max = 2 * PATH_MAX;
module_param_named(path_max, aa_g_path_max, aauint, S_IRUSR);

/* Determines how paranoid loading of policy is and how much verification
 * on the loaded policy is done.
 * DEPRECATED: read only as strict checking of load is always done now
 * that none root users (user namespaces) can load policy.
 */
bool aa_g_paranoid_load = true;
module_param_named(paranoid_load, aa_g_paranoid_load, aabool, S_IRUGO);

static int param_get_aaintbool(char *buffer, const struct kernel_param *kp);
static int param_set_aaintbool(const char *val, const struct kernel_param *kp);
#define param_check_aaintbool param_check_int
static const struct kernel_param_ops param_ops_aaintbool = {
	.set = param_set_aaintbool,
	.get = param_get_aaintbool
};
/* Boot time disable flag */
static int apparmor_enabled __lsm_ro_after_init = 1;
module_param_named(enabled, apparmor_enabled, aaintbool, 0444);

static int __init apparmor_enabled_setup(char *str)
{
	unsigned long enabled;
	int error = kstrtoul(str, 0, &enabled);
	if (!error)
		apparmor_enabled = enabled ? 1 : 0;
	return 1;
}

__setup("apparmor=", apparmor_enabled_setup);

/* set global flag turning off the ability to load policy */
static int param_set_aalockpolicy(const char *val, const struct kernel_param *kp)
{
	if (!apparmor_enabled)
		return -EINVAL;
	if (apparmor_initialized && !policy_admin_capable(NULL))
		return -EPERM;
	return param_set_bool(val, kp);
}

static int param_get_aalockpolicy(char *buffer, const struct kernel_param *kp)
{
	if (!apparmor_enabled)
		return -EINVAL;
	if (apparmor_initialized && !policy_view_capable(NULL))
		return -EPERM;
	return param_get_bool(buffer, kp);
}

static int param_set_aabool(const char *val, const struct kernel_param *kp)
{
	if (!apparmor_enabled)
		return -EINVAL;
	if (apparmor_initialized && !policy_admin_capable(NULL))
		return -EPERM;
	return param_set_bool(val, kp);
}

static int param_get_aabool(char *buffer, const struct kernel_param *kp)
{
	if (!apparmor_enabled)
		return -EINVAL;
	if (apparmor_initialized && !policy_view_capable(NULL))
		return -EPERM;
	return param_get_bool(buffer, kp);
}

static int param_set_aauint(const char *val, const struct kernel_param *kp)
{
	int error;

	if (!apparmor_enabled)
		return -EINVAL;
	/* file is ro but enforce 2nd line check */
	if (apparmor_initialized)
		return -EPERM;

	error = param_set_uint(val, kp);
	aa_g_path_max = max_t(uint32_t, aa_g_path_max, sizeof(union aa_buffer));
	pr_info("AppArmor: buffer size set to %d bytes\n", aa_g_path_max);

	return error;
}

static int param_get_aauint(char *buffer, const struct kernel_param *kp)
{
	if (!apparmor_enabled)
		return -EINVAL;
	if (apparmor_initialized && !policy_view_capable(NULL))
		return -EPERM;
	return param_get_uint(buffer, kp);
}

/* Can only be set before AppArmor is initialized (i.e. on boot cmdline). */
static int param_set_aaintbool(const char *val, const struct kernel_param *kp)
{
	struct kernel_param kp_local;
	bool value;
	int error;

	if (apparmor_initialized)
		return -EPERM;

	/* Create local copy, with arg pointing to bool type. */
	value = !!*((int *)kp->arg);
	memcpy(&kp_local, kp, sizeof(kp_local));
	kp_local.arg = &value;

	error = param_set_bool(val, &kp_local);
	if (!error)
		*((int *)kp->arg) = *((bool *)kp_local.arg);
	return error;
}

/*
 * To avoid changing /sys/module/apparmor/parameters/enabled from Y/N to
 * 1/0, this converts the "int that is actually bool" back to bool for
 * display in the /sys filesystem, while keeping it "int" for the LSM
 * infrastructure.
 */
static int param_get_aaintbool(char *buffer, const struct kernel_param *kp)
{
	struct kernel_param kp_local;
	bool value;

	/* Create local copy, with arg pointing to bool type. */
	value = !!*((int *)kp->arg);
	memcpy(&kp_local, kp, sizeof(kp_local));
	kp_local.arg = &value;

	return param_get_bool(buffer, &kp_local);
}

static int param_set_aacompressionlevel(const char *val,
					const struct kernel_param *kp)
{
	int error;

	if (!apparmor_enabled)
		return -EINVAL;
	if (apparmor_initialized)
		return -EPERM;

	error = param_set_int(val, kp);

	aa_g_rawdata_compression_level = clamp(aa_g_rawdata_compression_level,
					       Z_NO_COMPRESSION,
					       Z_BEST_COMPRESSION);
	pr_info("AppArmor: policy rawdata compression level set to %u\n",
		aa_g_rawdata_compression_level);

	return error;
}

static int param_get_aacompressionlevel(char *buffer,
					const struct kernel_param *kp)
{
	if (!apparmor_enabled)
		return -EINVAL;
	if (apparmor_initialized && !policy_view_capable(NULL))
		return -EPERM;
	return param_get_int(buffer, kp);
}

static int param_get_audit(char *buffer, const struct kernel_param *kp)
{
	if (!apparmor_enabled)
		return -EINVAL;
	if (apparmor_initialized && !policy_view_capable(NULL))
		return -EPERM;
	return sprintf(buffer, "%s", audit_mode_names[aa_g_audit]);
}

static int param_set_audit(const char *val, const struct kernel_param *kp)
{
	int i;

	if (!apparmor_enabled)
		return -EINVAL;
	if (!val)
		return -EINVAL;
	if (apparmor_initialized && !policy_admin_capable(NULL))
		return -EPERM;

	i = match_string(audit_mode_names, AUDIT_MAX_INDEX, val);
	if (i < 0)
		return -EINVAL;

	aa_g_audit = i;
	return 0;
}

static int param_get_mode(char *buffer, const struct kernel_param *kp)
{
	if (!apparmor_enabled)
		return -EINVAL;
	if (apparmor_initialized && !policy_view_capable(NULL))
		return -EPERM;

	return sprintf(buffer, "%s", aa_profile_mode_names[aa_g_profile_mode]);
}

static int param_set_mode(const char *val, const struct kernel_param *kp)
{
	int i;

	if (!apparmor_enabled)
		return -EINVAL;
	if (!val)
		return -EINVAL;
	if (apparmor_initialized && !policy_admin_capable(NULL))
		return -EPERM;

	i = match_string(aa_profile_mode_names, APPARMOR_MODE_NAMES_MAX_INDEX,
			 val);
	if (i < 0)
		return -EINVAL;

	aa_g_profile_mode = i;
	return 0;
}

char *aa_get_buffer(bool in_atomic)
{
	union aa_buffer *aa_buf;
	bool try_again = true;
	gfp_t flags = (GFP_KERNEL | __GFP_RETRY_MAYFAIL | __GFP_NOWARN);

retry:
	spin_lock(&aa_buffers_lock);
	if (buffer_count > reserve_count ||
	    (in_atomic && !list_empty(&aa_global_buffers))) {
		aa_buf = list_first_entry(&aa_global_buffers, union aa_buffer,
					  list);
		list_del(&aa_buf->list);
		buffer_count--;
		spin_unlock(&aa_buffers_lock);
		return &aa_buf->buffer[0];
	}
	if (in_atomic) {
		/*
		 * out of reserve buffers and in atomic context so increase
		 * how many buffers to keep in reserve
		 */
		reserve_count++;
		flags = GFP_ATOMIC;
	}
	spin_unlock(&aa_buffers_lock);

	if (!in_atomic)
		might_sleep();
	aa_buf = kmalloc(aa_g_path_max, flags);
	if (!aa_buf) {
		if (try_again) {
			try_again = false;
			goto retry;
		}
		pr_warn_once("AppArmor: Failed to allocate a memory buffer.\n");
		return NULL;
	}
	return &aa_buf->buffer[0];
}

void aa_put_buffer(char *buf)
{
	union aa_buffer *aa_buf;

	if (!buf)
		return;
	aa_buf = container_of(buf, union aa_buffer, buffer[0]);

	spin_lock(&aa_buffers_lock);
	list_add(&aa_buf->list, &aa_global_buffers);
	buffer_count++;
	spin_unlock(&aa_buffers_lock);
}

/*
 * AppArmor init functions
 */

/**
 * set_init_ctx - set a task context and profile on the first task.
 *
 * TODO: allow setting an alternate profile than unconfined
 */
static int __init set_init_ctx(void)
{
	struct cred *cred = (__force struct cred *)current->real_cred;

	set_cred_label(cred, aa_get_label(ns_unconfined(root_ns)));

	return 0;
}

static void destroy_buffers(void)
{
	union aa_buffer *aa_buf;

	spin_lock(&aa_buffers_lock);
	while (!list_empty(&aa_global_buffers)) {
		aa_buf = list_first_entry(&aa_global_buffers, union aa_buffer,
					 list);
		list_del(&aa_buf->list);
		spin_unlock(&aa_buffers_lock);
		kfree(aa_buf);
		spin_lock(&aa_buffers_lock);
	}
	spin_unlock(&aa_buffers_lock);
}

static int __init alloc_buffers(void)
{
	union aa_buffer *aa_buf;
	int i, num;

	/*
	 * A function may require two buffers at once. Usually the buffers are
	 * used for a short period of time and are shared. On UP kernel buffers
	 * two should be enough, with more CPUs it is possible that more
	 * buffers will be used simultaneously. The preallocated pool may grow.
	 * This preallocation has also the side-effect that AppArmor will be
	 * disabled early at boot if aa_g_path_max is extremly high.
	 */
	if (num_online_cpus() > 1)
		num = 4 + RESERVE_COUNT;
	else
		num = 2 + RESERVE_COUNT;

	for (i = 0; i < num; i++) {

		aa_buf = kmalloc(aa_g_path_max, GFP_KERNEL |
				 __GFP_RETRY_MAYFAIL | __GFP_NOWARN);
		if (!aa_buf) {
			destroy_buffers();
			return -ENOMEM;
		}
		aa_put_buffer(&aa_buf->buffer[0]);
	}
	return 0;
}

#ifdef CONFIG_SYSCTL
static int apparmor_dointvec(struct ctl_table *table, int write,
			     void *buffer, size_t *lenp, loff_t *ppos)
{
	if (!policy_admin_capable(NULL))
		return -EPERM;
	if (!apparmor_enabled)
		return -EINVAL;

	return proc_dointvec(table, write, buffer, lenp, ppos);
}

static struct ctl_path apparmor_sysctl_path[] = {
	{ .procname = "kernel", },
	{ }
};

static struct ctl_table apparmor_sysctl_table[] = {
	{
		.procname       = "unprivileged_userns_apparmor_policy",
		.data           = &unprivileged_userns_apparmor_policy,
		.maxlen         = sizeof(int),
		.mode           = 0600,
		.proc_handler   = apparmor_dointvec,
	},
	{
		.procname       = "apparmor_display_secid_mode",
		.data           = &apparmor_display_secid_mode,
		.maxlen         = sizeof(int),
		.mode           = 0600,
		.proc_handler   = apparmor_dointvec,
	},

	{ }
};

static int __init apparmor_init_sysctl(void)
{
	return register_sysctl_paths(apparmor_sysctl_path,
				     apparmor_sysctl_table) ? 0 : -ENOMEM;
}
#else
static inline int apparmor_init_sysctl(void)
{
	return 0;
}
#endif /* CONFIG_SYSCTL */

#if defined(CONFIG_NETFILTER) && defined(CONFIG_NETWORK_SECMARK)
static unsigned int apparmor_ip_postroute(void *priv,
					  struct sk_buff *skb,
					  const struct nf_hook_state *state)
{
	struct aa_sk_ctx *ctx;
	struct sock *sk;

	if (!skb->secmark)
		return NF_ACCEPT;

	sk = skb_to_full_sk(skb);
	if (sk == NULL)
		return NF_ACCEPT;

	ctx = aa_sock(sk);
	if (!apparmor_secmark_check(ctx->label, OP_SENDMSG, AA_MAY_SEND,
				    skb->secmark, sk))
		return NF_ACCEPT;

	return NF_DROP_ERR(-ECONNREFUSED);

}

static unsigned int apparmor_ipv4_postroute(void *priv,
					    struct sk_buff *skb,
					    const struct nf_hook_state *state)
{
	return apparmor_ip_postroute(priv, skb, state);
}

#if IS_ENABLED(CONFIG_IPV6)
static unsigned int apparmor_ipv6_postroute(void *priv,
					    struct sk_buff *skb,
					    const struct nf_hook_state *state)
{
	return apparmor_ip_postroute(priv, skb, state);
}
#endif

static const struct nf_hook_ops apparmor_nf_ops[] = {
	{
		.hook =         apparmor_ipv4_postroute,
		.pf =           NFPROTO_IPV4,
		.hooknum =      NF_INET_POST_ROUTING,
		.priority =     NF_IP_PRI_SELINUX_FIRST,
	},
#if IS_ENABLED(CONFIG_IPV6)
	{
		.hook =         apparmor_ipv6_postroute,
		.pf =           NFPROTO_IPV6,
		.hooknum =      NF_INET_POST_ROUTING,
		.priority =     NF_IP6_PRI_SELINUX_FIRST,
	},
#endif
};

static int __net_init apparmor_nf_register(struct net *net)
{
	int ret;

	ret = nf_register_net_hooks(net, apparmor_nf_ops,
				    ARRAY_SIZE(apparmor_nf_ops));
	return ret;
}

static void __net_exit apparmor_nf_unregister(struct net *net)
{
	nf_unregister_net_hooks(net, apparmor_nf_ops,
				ARRAY_SIZE(apparmor_nf_ops));
}

static struct pernet_operations apparmor_net_ops = {
	.init = apparmor_nf_register,
	.exit = apparmor_nf_unregister,
};

static int __init apparmor_nf_ip_init(void)
{
	int err;

	if (!apparmor_enabled)
		return 0;

	err = register_pernet_subsys(&apparmor_net_ops);
	if (err)
		panic("Apparmor: register_pernet_subsys: error %d\n", err);

	return 0;
}
__initcall(apparmor_nf_ip_init);
#endif

static int __init apparmor_init(void)
{
	int error;

	aa_secids_init();

	error = aa_setup_dfa_engine();
	if (error) {
		AA_ERROR("Unable to setup dfa engine\n");
		goto alloc_out;
	}

	error = aa_alloc_root_ns();
	if (error) {
		AA_ERROR("Unable to allocate default profile namespace\n");
		goto alloc_out;
	}

	error = apparmor_init_sysctl();
	if (error) {
		AA_ERROR("Unable to register sysctls\n");
		goto alloc_out;

	}

	error = alloc_buffers();
	if (error) {
		AA_ERROR("Unable to allocate work buffers\n");
		goto alloc_out;
	}

	error = set_init_ctx();
	if (error) {
		AA_ERROR("Failed to set context on init task\n");
		aa_free_root_ns();
		goto buffers_out;
	}
	security_add_hooks(apparmor_hooks, ARRAY_SIZE(apparmor_hooks),
				&apparmor_lsmid);

	/* Report that AppArmor successfully initialized */
	apparmor_initialized = 1;
	if (aa_g_profile_mode == APPARMOR_COMPLAIN)
		aa_info_message("AppArmor initialized: complain mode enabled");
	else if (aa_g_profile_mode == APPARMOR_KILL)
		aa_info_message("AppArmor initialized: kill mode enabled");
	else
		aa_info_message("AppArmor initialized");

	return error;

buffers_out:
	destroy_buffers();
alloc_out:
	aa_destroy_aafs();
	aa_teardown_dfa_engine();

	apparmor_enabled = false;
	return error;
}

DEFINE_LSM(apparmor) = {
	.name = "apparmor",
	.flags = LSM_FLAG_LEGACY_MAJOR,
	.enabled = &apparmor_enabled,
	.blobs = &apparmor_blob_sizes,
	.init = apparmor_init,
};
