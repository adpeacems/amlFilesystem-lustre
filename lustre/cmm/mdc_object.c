/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/cmm/mdc_object.c
 *  Lustre Cluster Metadata Manager (cmm)
 *
 *  Copyright (c) 2006 Cluster File Systems, Inc.
 *   Author: Mike Pershin <tappro@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#define DEBUG_SUBSYSTEM S_MDS
#include <obd_support.h>
#include <lustre_lib.h>
#include <obd_class.h>
#include "mdc_internal.h"

static struct md_object_operations mdc_mo_ops;
static struct md_dir_operations mdc_dir_ops;
static struct lu_object_operations mdc_obj_ops;

#ifdef CMM_CODE
extern struct lu_context_key mdc_thread_key;

struct lu_object *mdc_object_alloc(const struct lu_context *ctx,
                                   const struct lu_object_header *hdr,
                                   struct lu_device *ld)
{
	struct mdc_object *mco;
        ENTRY;

	OBD_ALLOC_PTR(mco);
	if (mco != NULL) {
		struct lu_object *lo;

		lo = &mco->mco_obj.mo_lu;
                lu_object_init(lo, NULL, ld);
                mco->mco_obj.mo_ops = &mdc_mo_ops;
                mco->mco_obj.mo_dir_ops = &mdc_dir_ops;
                lo->lo_ops = &mdc_obj_ops;
                RETURN(lo);
	} else
		RETURN(NULL);
}

static void mdc_object_free(const struct lu_context *ctx, struct lu_object *lo)
{
        struct mdc_object *mco = lu2mdc_obj(lo);
	lu_object_fini(lo);
        OBD_FREE_PTR(mco);
}

static int mdc_object_init(const struct lu_context *ctx, struct lu_object *lo)
{
        ENTRY;

        RETURN(0);
}

static void mdc_object_release(const struct lu_context *ctx,
                               struct lu_object *lo)
{
}

static int mdc_object_exists(const struct lu_context *ctx, struct lu_object *lo)
{
        /* we don't know does it exists or not - but suppose that it does*/
        return 1;
}

static int mdc_object_print(const struct lu_context *ctx,
                            struct seq_file *f, const struct lu_object *lo)
{
	return seq_printf(f, LUSTRE_MDC0_NAME"-object@%p", lo);
}

static struct lu_object_operations mdc_obj_ops = {
        .loo_object_init    = mdc_object_init,
	.loo_object_release = mdc_object_release,
        .loo_object_free    = mdc_object_free,
	.loo_object_print   = mdc_object_print,
	.loo_object_exists  = mdc_object_exists
};

/* md_object_operations */
static int mdc_object_create(const struct lu_context *ctx,
                             struct md_object *mo, struct lu_attr *attr)
{
        struct mdc_device *mc = md2mdc_dev(md_device_get(mo));
        struct mdc_thread_info *mci;
        int rc;
        ENTRY;

        mci = lu_context_get_key(ctx, &mdc_thread_key);
        LASSERT(mci);

        mci->mci_opdata.fid1 = *lu_object_fid(&mo->mo_lu);
        mci->mci_opdata.fid2 = { 0 };
        mci->mci_opdata.mod_time = attr->la_mtime;
        mci->mci_opdata.name = NULL;
        mci->mci_opdata.namelen = 0;

        rc = md_create(mc->mc_desc.cl_exp, &mci->mci_opdata, NULL, 0,
                       attr->la_mode, attr->la_uid, attr->la_gid, 0, 0,
                       &mci->mci_req);

        RETURN(rc);
}
static int mdc_ref_add(const struct lu_context *ctx, struct md_object *mo)
{
        struct mdc_device *mc = md2mdc_dev(md_device_get(mo));
        struct mdc_thread_info *mci;
        int rc;
        ENTRY;

        mci = lu_context_get_key(ctx, &mdc_thread_key);
        LASSERT(mci);

        mci->mci_opdata.fid1 = *lu_object_fid(&mo->mo_lu);
        mci->mci_opdata.fid2 = { 0 };
        mci->mci_opdata.mod_time = attr->la_mtime;
        mci->mci_opdata.name = NULL;
        mci->mci_opdata.namelen = 0;

        rc = md_link(mc->mc_desc.cl_exp, &mci->mci_opdata, &mci->mci_req);

        RETURN(rc);
}

static int mdc_ref_del(const struct lu_context *ctx, struct md_object *mo)
{
        struct mdc_device *mc = md2mdc_dev(md_device_get(mo));
        struct mdc_thread_info *mci;
        int rc;
        ENTRY;

        mci = lu_context_get_key(ctx, &mdc_thread_key);
        LASSERT(mci);

        mci->mci_opdata.fid1 = *lu_object_fid(&mo->mo_lu);
        mci->mci_opdata.fid2 = { 0 };
        mci->mci_opdata.mod_time = attr->la_mtime;
        mci->mci_opdata.name = NULL;
        mci->mci_opdata.namelen = 0;

        rc = md_unlink(mc->mc_desc.cl_exp, &mci->mci_opdata, &mci->mci_req);

        RETURN(rc);
}

static struct md_object_operations mdc_mo_ops = {
        .moo_object_create  = mdc_object_create,
        .moo_ref_add        = mdc_ref_add,
        .moo_ref_del        = mdc_ref_del,
};

/* md_dir_operations */
static int mdc_rename_tgt(const struct lu_context *ctx,
                          struct md_object *mo_p, struct md_object *mo_t,
                          const struct lu_fid *lf, const char *name)
{
        struct mdc_device *mc = md2mdc_dev(md_device_get(mo));
        struct mdc_thread_info *mci;
        int rc;
        ENTRY;

        mci = lu_context_get_key(ctx, &mdc_thread_key);
        LASSERT(mci);

        mci->mci_opdata.fid1 = *lu_object_fid(&mo_p->mo_lu);
        mci->mci_opdata.fid2 = *lf;
        mci->mci_opdata.mod_time = attr->la_mtime;

        rc = md_rename(mc->mc_desc.cl_exp, &mci->mci_opdata, NULL, 0,
                       name, strlen(name), &mci->mci_req);

        RETURN(rc);
}

static struct md_dir_operations mdc_dir_ops = {
        .mdo_rename_tgt  = mdc_rename_tgt,
};

#else /* CMM_CODE */
struct lu_object *mdc_object_alloc(const struct lu_context *ctx,
                                   const struct lu_object_header *hdr,
                                   struct lu_device *ld)
{
	struct mdc_object *mco;
        ENTRY;

	OBD_ALLOC_PTR(mco);
	if (mco != NULL) {
		struct lu_object *lo;

		lo = &mco->mco_obj.mo_lu;
                lu_object_init(lo, NULL, ld);
                mco->mco_obj.mo_ops = &mdc_mo_ops;
                mco->mco_obj.mo_dir_ops = &mdc_dir_ops;
                lo->lo_ops = &mdc_obj_ops;
                RETURN(lo);
	} else
		RETURN(NULL);
}

static int mdc_object_init(const struct lu_context *ctx, struct lu_object *lo)
{
	//struct mdc_device *d = lu2mdc_dev(o->lo_dev);
	//struct lu_device  *under;
        //const struct lu_fid     *fid = lu_object_fid(o);

        ENTRY;

        RETURN(0);
}

static void mdc_object_free(const struct lu_context *ctx, struct lu_object *lo)
{
        struct mdc_object *mco = lu2mdc_obj(lo);
	lu_object_fini(lo);
        OBD_FREE_PTR(mco);
}

static void mdc_object_release(const struct lu_context *ctx,
                               struct lu_object *lo)
{
        return;
}

static int mdc_object_exists(const struct lu_context *ctx, struct lu_object *lo)
{
        return 0;
}

static int mdc_object_print(const struct lu_context *ctx,
                            struct seq_file *f, const struct lu_object *lo)
{
	return seq_printf(f, LUSTRE_MDC0_NAME"-object@%p", lo);
}

static int mdc_object_create(const struct lu_context *ctx,
                             struct md_object *mo, struct lu_attr *attr)
{
        struct mdc_device *mc = md2mdc_dev(md_device_get(mo));
        struct obd_export *exp = mc->mc_desc.cl_exp;
        struct ptlrpc_request *req;
        struct md_op_data op_data = {
                .fid1 = mo->mo_lu.lo_header->loh_fid,
                .fid2 = { 0 },
                .mod_time = attr->la_mtime,
                .name = NULL,
                .namelen = 0,
        };
        int rc;

        rc = md_create(exp, &op_data, NULL, 0, attr->la_mode, attr->la_uid,
                       attr->la_gid, 0, 0, &req);
        RETURN(rc);
}

static struct md_dir_operations mdc_dir_ops = {
};

static struct md_object_operations mdc_mo_ops = {
        .moo_object_create  = mdc_object_create
};

static struct lu_object_operations mdc_obj_ops = {
        .loo_object_init    = mdc_object_init,
	.loo_object_release = mdc_object_release,
        .loo_object_free    = mdc_object_free,
	.loo_object_print   = mdc_object_print,
	.loo_object_exists  = mdc_object_exists
};
#endif /* CMM_CODE */
