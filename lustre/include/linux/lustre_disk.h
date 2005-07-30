/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2001 Cluster File Systems, Inc. <braam@clusterfs.com>
 *   Author: Nathan Rutman <nathan@clusterfs.com>
 *   Author: Lin Song Tao <lincent@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 * Lustre disk format definitions.
 */

#ifndef _LUSTRE_DISK_H
#define _LUSTRE_DISK_H

#include <linux/types.h>
#include <portals/types.h>


/****************** persistent mount data *********************/

/* Persistent mount data are stored on the disk in this file.
   Used before the setup llog can be read. */
#define MOUNT_CONFIGS_DIR "CONFIGS/"
#define MOUNT_DATA_FILE   MOUNT_CONFIGS_DIR"mountdata"

#define LDD_MAGIC 0xbabb0001

#define LDD_SV_TYPE_MDT  0x0001
#define LDD_SV_TYPE_OST  0x0002
#define LDD_SV_TYPE_MGMT 0x0004

enum ldd_mount_type {
        LDD_MT_EXT3 = 0, 
        LDD_MT_LDISKFS,
        LDD_MT_SMFS,   
        LDD_MT_REISERFS,
        LDD_MT_LAST
};
       
static inline char *mt_str(enum ldd_mount_type mt)
{
        static char *mount_type_string[] = {
                "ext3",
                "ldiskfs",
                "smfs",
                "reiserfs",
        };
        //LASSERT(mt < LDD_MT_LAST);
        return mount_type_string[mt];
}

struct host_desc {
        ptl_nid_t primary; 
        ptl_nid_t backup;
};

struct lustre_disk_data {
        __u32     ldd_magic;
        __u32     ldd_flags;           /* LDD_SV_TYPE */
        struct host_desc ldd_mgmtnid;  /* mgmt nid; lmd can override */
        char      ldd_fsname[64];      /* filesystem this server is part of */
        char      ldd_svname[64];      /* this server's name (lustre-mdt0001) */
        enum ldd_mount_type ldd_mount_type; /* target fs type LDD_MT_* */
        char      ldd_mount_opts[128]; /* target fs mount opts */
};
        
#define IS_MDT(data)   ((data)->ldd_flags & LDD_SV_TYPE_MDT)
#define IS_OST(data)   ((data)->ldd_flags & LDD_SV_TYPE_OST)
#define IS_MGMT(data)  ((data)->ldd_flags & LDD_SV_TYPE_MGMT)
#define MT_STR(data)   mt_str((data)->ldd_mount_type)

/****************** mount command *********************/

/* Passed by mount - no persistent info here */
struct lustre_mount_data {
        __u32     lmd_magic;
        __u32     lmd_flags;          /* lustre mount flags */
        struct host_desc lmd_mgmtnid; /* mgmt nid */
        //struct lustre_disk_data *lmd_ldd; /* in-mem copy of ldd */
        char      lmd_dev[128];       /* device or file system name */
        char      lmd_mtpt[128];      /* mount point (for client overmount) */
        char      lmd_opts[256];      /* lustre mount options (as opposed to 
                                         _device_ mount options) */
};

#define LMD_FLG_FLOCK   0x0001  /* Enable flock */
#define LMD_FLG_RECOVER 0x0002  /* Allow recovery */
#define LMD_FLG_MNTCNF  0x1000  /* MountConf compat */
#define LMD_FLG_CLIENT  0x2000  /* Mounting a client only; no real device */

/* 2nd half is for old clients */
#define lmd_is_client(x) \
        (((x)->lmd_flags & LMD_FLG_CLIENT) || \
        (!((x)->lmd_flags & LMD_FLG_MNTCNF))) 


/****************** mkfs command *********************/

#define MO_IS_LOOP     0x01
#define MO_FORCEFORMAT 0x02

/* used to describe the options to format the lustre disk, not persistent */
struct mkfs_opts {
        struct lustre_disk_data mo_ldd; /* to be written in MOUNT_DATA_FILE */
        char  mo_mount_type_string[20]; /* "ext3", "ldiskfs", ... */
        char  mo_device[128];           /* disk device name */
        char  mo_mkfsopts[128];         /* options to the backing-store mkfs */
        long  mo_device_sz;
        int   mo_flags; 
        /* Below here is required for mdt,ost,or client logs */
        struct host_desc mo_hostnid;    /* server nid + failover - need to know
                                           for client log */
        int   mo_stripe_sz;
        int   mo_stripe_count;
        int   mo_stripe_pattern;
        __u16 mo_index;                 /* stripe index for osts, pool index
                                           for pooled mdts.  index will be put
                                           in lr_server_data */
        int   mo_timeout;               /* obd timeout */
};

/****************** last_rcvd file *********************/

#define LAST_RCVD "last_rcvd"
#define LR_SERVER_SIZE    512

/* Data stored per server at the head of the last_rcvd file.  In le32 order.
   This should be common to filter_internal.h, lustre_mds.h */
struct lr_server_data {
        __u8  lsd_uuid[40];        /* server UUID */
        __u64 lsd_unused;          /* was lsd_last_objid - don't use for now */
        __u64 lsd_last_transno;    /* last completed transaction ID */
        __u64 lsd_mount_count;     /* FILTER incarnation number */
        __u32 lsd_feature_compat;  /* compatible feature flags */
        __u32 lsd_feature_rocompat;/* read-only compatible feature flags */
        __u32 lsd_feature_incompat;/* incompatible feature flags */
        __u32 lsd_server_size;     /* size of server data area */
        __u32 lsd_client_start;    /* start of per-client data area */
        __u16 lsd_client_size;     /* size of per-client data area */
        __u16 lsd_subdir_count;    /* number of subdirectories for objects */
        __u64 lsd_catalog_oid;     /* recovery catalog object id */
        __u32 lsd_catalog_ogen;    /* recovery catalog inode generation */
        __u8  lsd_peeruuid[40];    /* UUID of MDS associated with this OST */
        __u32 lsd_index;           /* target index (stripe index for ost)*/
        __u8  lsd_padding[LR_SERVER_SIZE - 144];
};

#ifdef __KERNEL__
/****************** superblock additional info *********************/
struct ll_sb_info;

struct lustre_sb_info {
        int                       lsi_flags;
        //struct lvfs_run_ctxt      lsi_ctxt;    /* mount context */
        struct obd_device        *lsi_mgc;     /* mgmt cli obd */
        struct lustre_mount_data *lsi_lmd;     /* mount command info */
        struct lustre_disk_data  *lsi_ldd;     /* mount info on-disk */
        //struct fsfilt_operations *lsi_fsops;
        struct ll_sb_info        *lsi_llsbi;   /* add'l client sbi info */
};

#define LSI_SERVER                       0x00000001
#define LSI_UMOUNT_FORCE                 0x00000010
#define LSI_UMOUNT_FAILOVER              0x00000020

#if  (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,0))
# define    s2sbi(sb)        ((struct lustre_sb_info *)((sb)->s_fs_info))
# define    s2sbi_nocast(sb) ((sb)->s_fs_info)
#else  /* 2.4 here */
# define    s2sbi(sb)        ((struct lustre_sb_info *)((sb)->u.generic_sbp))
# define    s2sbi_nocast(sb) ((sb)->u.generic_sbp)
#endif

#endif /* __KERNEL__ */

#endif // _LUSTRE_DISK_H
