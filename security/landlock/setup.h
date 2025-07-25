/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Landlock LSM - Security framework setup
 *
 * Copyright © 2016-2020 Mickaël Salaün <mic@digikod.net>
 * Copyright © 2018-2020 ANSSI
 */

#ifndef _SECURITY_LANDLOCK_SETUP_H
#define _SECURITY_LANDLOCK_SETUP_H

#include <linux/lsm_hooks.h>

extern const int landlock_abi_version;

extern bool landlock_initialized;
extern int landlock_errata;

extern struct lsm_blob_sizes landlock_blob_sizes;
extern struct lsm_id landlock_lsmid;

#endif /* _SECURITY_LANDLOCK_SETUP_H */
