/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2004 Cluster File Systems, Inc.
 *   Author: Eric Barton <eric@bartonsoftware.com>
 *   Author: Frank Zago <fzago@systemfabricworks.com>
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
 */

#include "vibnal.h"

nal_t                   kibnal_api;
ptl_handle_ni_t         kibnal_ni;
kib_data_t              kibnal_data;
kib_tunables_t          kibnal_tunables;

#ifdef CONFIG_SYSCTL
#define IBNAL_SYSCTL             202

#define IBNAL_SYSCTL_TIMEOUT     1

static ctl_table kibnal_ctl_table[] = {
        {IBNAL_SYSCTL_TIMEOUT, "timeout", 
         &kibnal_tunables.kib_io_timeout, sizeof (int),
         0644, NULL, &proc_dointvec},
        { 0 }
};

static ctl_table kibnal_top_ctl_table[] = {
        {IBNAL_SYSCTL, "vibnal", NULL, 0, 0555, kibnal_ctl_table},
        { 0 }
};
#endif

void
kibnal_pause(int ticks)
{
        set_current_state(TASK_UNINTERRUPTIBLE);
        schedule_timeout(ticks);
}

__u32 
kibnal_cksum (void *ptr, int nob)
{
        char  *c  = ptr;
        __u32  sum = 0;

        while (nob-- > 0)
                sum = ((sum << 1) | (sum >> 31)) + *c++;

        /* ensure I don't return 0 (== no checksum) */
        return (sum == 0) ? 1 : sum;
}

void
kibnal_init_msg(kib_msg_t *msg, int type, int body_nob)
{
        msg->ibm_type = type;
        msg->ibm_nob  = offsetof(kib_msg_t, ibm_u) + body_nob;
}

void
kibnal_pack_msg(kib_msg_t *msg, int credits, ptl_nid_t dstnid, __u64 dststamp)
{
        /* CAVEAT EMPTOR! all message fields not set here should have been
         * initialised previously. */
        msg->ibm_magic    = IBNAL_MSG_MAGIC;
        msg->ibm_version  = IBNAL_MSG_VERSION;
        /*   ibm_type */
        msg->ibm_credits  = credits;
        /*   ibm_nob */
        msg->ibm_cksum    = 0;
        msg->ibm_srcnid   = kibnal_lib.libnal_ni.ni_pid.nid;
        msg->ibm_srcstamp = kibnal_data.kib_incarnation;
        msg->ibm_dstnid   = dstnid;
        msg->ibm_dststamp = dststamp;
#if IBNAL_CKSUM
        /* NB ibm_cksum zero while computing cksum */
        msg->ibm_cksum    = kibnal_cksum(msg, msg->ibm_nob);
#endif
}

int
kibnal_unpack_msg(kib_msg_t *msg, int nob)
{
        const int hdr_size = offsetof(kib_msg_t, ibm_u);
        __u32     msg_cksum;
        int       flip;
        int       msg_nob;
        int       i;
        int       n;

        /* 6 bytes are enough to have received magic + version */
        if (nob < 6) {
                CERROR("Short message: %d\n", nob);
                return -EPROTO;
        }

        if (msg->ibm_magic == IBNAL_MSG_MAGIC) {
                flip = 0;
        } else if (msg->ibm_magic == __swab32(IBNAL_MSG_MAGIC)) {
                flip = 1;
        } else {
                CERROR("Bad magic: %08x\n", msg->ibm_magic);
                return -EPROTO;
        }

        if (msg->ibm_version != 
            (flip ? __swab16(IBNAL_MSG_VERSION) : IBNAL_MSG_VERSION)) {
                CERROR("Bad version: %d\n", msg->ibm_version);
                return -EPROTO;
        }

        if (nob < hdr_size) {
                CERROR("Short message: %d\n", nob);
                return -EPROTO;
        }

        msg_nob = flip ? __swab32(msg->ibm_nob) : msg->ibm_nob;
        if (msg_nob > nob) {
                CERROR("Short message: got %d, wanted %d\n", nob, msg_nob);
                return -EPROTO;
        }

        /* checksum must be computed with ibm_cksum zero and BEFORE anything
         * gets flipped */
        msg_cksum = flip ? __swab32(msg->ibm_cksum) : msg->ibm_cksum;
        msg->ibm_cksum = 0;
        if (msg_cksum != 0 &&
            msg_cksum != kibnal_cksum(msg, msg_nob)) {
                CERROR("Bad checksum\n");
                return -EPROTO;
        }
        msg->ibm_cksum = msg_cksum;
        
        if (flip) {
                /* leave magic unflipped as a clue to peer endianness */
                __swab16s(&msg->ibm_version);
                CLASSERT (sizeof(msg->ibm_type) == 1);
                CLASSERT (sizeof(msg->ibm_credits) == 1);
                msg->ibm_nob = msg_nob;
                __swab64s(&msg->ibm_srcnid);
                __swab64s(&msg->ibm_srcstamp);
                __swab64s(&msg->ibm_dstnid);
                __swab64s(&msg->ibm_dststamp);
        }
        
        if (msg->ibm_srcnid == PTL_NID_ANY) {
                CERROR("Bad src nid: "LPX64"\n", msg->ibm_srcnid);
                return -EPROTO;
        }

        switch (msg->ibm_type) {
        default:
                CERROR("Unknown message type %x\n", msg->ibm_type);
                return -EPROTO;
                
        case IBNAL_MSG_NOOP:
                break;

        case IBNAL_MSG_IMMEDIATE:
                if (msg_nob < offsetof(kib_msg_t, ibm_u.immediate.ibim_payload[0])) {
                        CERROR("Short IMMEDIATE: %d(%d)\n", msg_nob,
                               (int)offsetof(kib_msg_t, ibm_u.immediate.ibim_payload[0]));
                        return -EPROTO;
                }
                break;

        case IBNAL_MSG_PUT_REQ:
                /* CAVEAT EMPTOR!  We don't actually put ibprm_rd on the wire;
                 * it's just there to remember the source buffers while we wait
                 * for the PUT_ACK */
                if (msg_nob < offsetof(kib_msg_t, ibm_u.putreq.ibprm_rd)) {
                        CERROR("Short PUT_REQ: %d(%d)\n", msg_nob,
                               (int)(hdr_size + sizeof(msg->ibm_u.putreq)));
                        return -EPROTO;
                }
                break;

        case IBNAL_MSG_PUT_ACK:
                if (msg_nob < offsetof(kib_msg_t, ibm_u.putack.ibpam_rd.rd_frags[0])) {
                        CERROR("Short PUT_ACK: %d(%d)\n", msg_nob,
                               (int)offsetof(kib_msg_t, ibm_u.putack.ibpam_rd.rd_frags[0]));
                        return -EPROTO;
                }

                if (flip) {
                        __swab32s(&msg->ibm_u.putack.ibpam_rd.rd_key);
                        __swab32s(&msg->ibm_u.putack.ibpam_rd.rd_nfrag);
                }
                
                n = msg->ibm_u.putack.ibpam_rd.rd_nfrag;
                if (n <= 0 || n > IBNAL_MAX_RDMA_FRAGS) {
                        CERROR("Bad PUT_ACK nfrags: %d, should be 0 < n <= %d\n", 
                               n, IBNAL_MAX_RDMA_FRAGS);
                        return -EPROTO;
                }
                
                if (msg_nob < offsetof(kib_msg_t, ibm_u.putack.ibpam_rd.rd_frags[n])) {
                        CERROR("Short PUT_ACK: %d(%d)\n", msg_nob,
                               (int)offsetof(kib_msg_t, ibm_u.putack.ibpam_rd.rd_frags[n]));
                        return -EPROTO;
                }

                if (flip)
                        for (i = 0; i < n; i++) {
                                __swab32s(&msg->ibm_u.putack.ibpam_rd.rd_frags[i].rf_nob);
                                __swab32s(&msg->ibm_u.putack.ibpam_rd.rd_frags[i].rf_addr_lo);
                                __swab32s(&msg->ibm_u.putack.ibpam_rd.rd_frags[i].rf_addr_hi);
                        }
                break;

        case IBNAL_MSG_GET_REQ:
                if (msg_nob < hdr_size + sizeof(msg->ibm_u.get)) {
                        CERROR("Short GET_REQ: %d(%d)\n", msg_nob,
                               (int)(hdr_size + sizeof(msg->ibm_u.get)));
                        return -EPROTO;
                }
                if (flip) {
                        __swab32s(&msg->ibm_u.get.ibgm_rd.rd_key);
                        __swab32s(&msg->ibm_u.get.ibgm_rd.rd_nfrag);
                }

                n = msg->ibm_u.get.ibgm_rd.rd_nfrag;
                if (n <= 0 || n > IBNAL_MAX_RDMA_FRAGS) {
                        CERROR("Bad GET_REQ nfrags: %d, should be 0 < n <= %d\n", 
                               n, IBNAL_MAX_RDMA_FRAGS);
                        return -EPROTO;
                }
                
                if (msg_nob < offsetof(kib_msg_t, ibm_u.get.ibgm_rd.rd_frags[n])) {
                        CERROR("Short GET_REQ: %d(%d)\n", msg_nob,
                               (int)offsetof(kib_msg_t, ibm_u.get.ibgm_rd.rd_frags[n]));
                        return -EPROTO;
                }
                
                if (flip)
                        for (i = 0; i < msg->ibm_u.get.ibgm_rd.rd_nfrag; i++) {
                                __swab32s(&msg->ibm_u.get.ibgm_rd.rd_frags[i].rf_nob);
                                __swab32s(&msg->ibm_u.get.ibgm_rd.rd_frags[i].rf_addr_lo);
                                __swab32s(&msg->ibm_u.get.ibgm_rd.rd_frags[i].rf_addr_hi);
                        }
                break;

        case IBNAL_MSG_PUT_NAK:
        case IBNAL_MSG_PUT_DONE:
        case IBNAL_MSG_GET_DONE:
                if (msg_nob < hdr_size + sizeof(msg->ibm_u.completion)) {
                        CERROR("Short RDMA completion: %d(%d)\n", msg_nob,
                               (int)(hdr_size + sizeof(msg->ibm_u.completion)));
                        return -EPROTO;
                }
                if (flip)
                        __swab32s(&msg->ibm_u.completion.ibcm_status);
                break;

        case IBNAL_MSG_CONNREQ:
        case IBNAL_MSG_CONNACK:
                if (msg_nob < hdr_size + sizeof(msg->ibm_u.connparams)) {
                        CERROR("Short connreq/ack: %d(%d)\n", msg_nob,
                               (int)(hdr_size + sizeof(msg->ibm_u.connparams)));
                        return -EPROTO;
                }
                if (flip) {
                        __swab32s(&msg->ibm_u.connparams.ibcp_queue_depth);
                        __swab32s(&msg->ibm_u.connparams.ibcp_max_msg_size);
                        __swab32s(&msg->ibm_u.connparams.ibcp_max_frags);
                }
                break;
        }
        return 0;
}

int
kibnal_set_mynid(ptl_nid_t nid)
{
        static cm_listen_data_t info;           /* protected by kib_nid_mutex */

        lib_ni_t        *ni = &kibnal_lib.libnal_ni;
        int              rc;
        cm_return_t      cmrc;

        CDEBUG(D_IOCTL, "setting mynid to "LPX64" (old nid="LPX64")\n",
               nid, ni->ni_pid.nid);

        down (&kibnal_data.kib_nid_mutex);

        if (nid == ni->ni_pid.nid) {
                /* no change of NID */
                up (&kibnal_data.kib_nid_mutex);
                return (0);
        }

        CDEBUG(D_NET, "NID "LPX64"("LPX64")\n", ni->ni_pid.nid, nid);

        if (kibnal_data.kib_listen_handle != NULL) {
                cmrc = cm_cancel(kibnal_data.kib_listen_handle);
                if (cmrc != cm_stat_success)
                        CERROR ("Error %d stopping listener\n", cmrc);

                kibnal_pause(HZ/10);            /* ensure no more callbacks */
        
                cmrc = cm_destroy_cep(kibnal_data.kib_listen_handle);
                if (cmrc != vv_return_ok)
                        CERROR ("Error %d destroying CEP\n", cmrc);

                kibnal_data.kib_listen_handle = NULL;
        }

        /* Change NID.  NB queued passive connection requests (if any) will be
         * rejected with an incorrect destination NID */
        ni->ni_pid.nid = nid;
        kibnal_data.kib_incarnation++;
        mb();

        /* Delete all existing peers and their connections after new
         * NID/incarnation set to ensure no old connections in our brave
         * new world. */
        kibnal_del_peer (PTL_NID_ANY, 0);

        if (ni->ni_pid.nid != PTL_NID_ANY) {    /* got a new NID to install */
                kibnal_data.kib_listen_handle = 
                        cm_create_cep(cm_cep_transp_rc);
                if (kibnal_data.kib_listen_handle == NULL) {
                        CERROR ("Can't create listen CEP\n");
                        rc = -ENOMEM;
                        goto failed_0;
                }

                CDEBUG(D_NET, "Created CEP %p for listening\n", 
                       kibnal_data.kib_listen_handle);

                memset(&info, 0, sizeof(info));
                info.listen_addr.end_pt.sid = kibnal_data.kib_svc_id;

                cmrc = cm_listen(kibnal_data.kib_listen_handle, &info,
                                 kibnal_listen_callback, NULL);
                if (cmrc != 0) {
                        CERROR ("cm_listen error: %d\n", cmrc);
                        rc = -EINVAL;
                        goto failed_1;
                }
        }

        up (&kibnal_data.kib_nid_mutex);
        return (0);

 failed_1:
        cmrc = cm_destroy_cep(kibnal_data.kib_listen_handle);
        LASSERT (cmrc == cm_stat_success);
        kibnal_data.kib_listen_handle = NULL;
 failed_0:
        ni->ni_pid.nid = PTL_NID_ANY;
        kibnal_data.kib_incarnation++;
        mb();
        kibnal_del_peer (PTL_NID_ANY, 0);
        up (&kibnal_data.kib_nid_mutex);
        return rc;
}

kib_peer_t *
kibnal_create_peer (ptl_nid_t nid)
{
        kib_peer_t *peer;

        LASSERT (nid != PTL_NID_ANY);

        PORTAL_ALLOC(peer, sizeof (*peer));
        if (peer == NULL) {
                CERROR("Canot allocate perr\n");
                return (NULL);
        }

        memset(peer, 0, sizeof(*peer));         /* zero flags etc */

        peer->ibp_nid = nid;
        atomic_set (&peer->ibp_refcount, 1);    /* 1 ref for caller */

        INIT_LIST_HEAD (&peer->ibp_list);       /* not in the peer table yet */
        INIT_LIST_HEAD (&peer->ibp_conns);
        INIT_LIST_HEAD (&peer->ibp_tx_queue);

        peer->ibp_reconnect_time = jiffies;
        peer->ibp_reconnect_interval = IBNAL_MIN_RECONNECT_INTERVAL;

        atomic_inc (&kibnal_data.kib_npeers);
        if (atomic_read(&kibnal_data.kib_npeers) <= IBNAL_CONCURRENT_PEERS)
                return peer;
        
        CERROR("Too many peers: CQ will overflow\n");
        kibnal_peer_decref(peer);
        return NULL;
}

void
kibnal_destroy_peer (kib_peer_t *peer)
{

        LASSERT (atomic_read (&peer->ibp_refcount) == 0);
        LASSERT (peer->ibp_persistence == 0);
        LASSERT (!kibnal_peer_active(peer));
        LASSERT (peer->ibp_connecting == 0);
        LASSERT (list_empty (&peer->ibp_conns));
        LASSERT (list_empty (&peer->ibp_tx_queue));
        
        PORTAL_FREE (peer, sizeof (*peer));

        /* NB a peer's connections keep a reference on their peer until
         * they are destroyed, so we can be assured that _all_ state to do
         * with this peer has been cleaned up when its refcount drops to
         * zero. */
        atomic_dec (&kibnal_data.kib_npeers);
}

/* the caller is responsible for accounting for the additional reference
 * that this creates */
kib_peer_t *
kibnal_find_peer_locked (ptl_nid_t nid)
{
        struct list_head *peer_list = kibnal_nid2peerlist (nid);
        struct list_head *tmp;
        kib_peer_t       *peer;

        list_for_each (tmp, peer_list) {

                peer = list_entry (tmp, kib_peer_t, ibp_list);

                LASSERT (peer->ibp_persistence != 0 || /* persistent peer */
                         peer->ibp_connecting != 0 || /* creating conns */
                         !list_empty (&peer->ibp_conns));  /* active conn */

                if (peer->ibp_nid != nid)
                        continue;

                CDEBUG(D_NET, "got peer [%p] -> "LPX64" (%d)\n",
                       peer, nid, atomic_read (&peer->ibp_refcount));
                return (peer);
        }
        return (NULL);
}

void
kibnal_unlink_peer_locked (kib_peer_t *peer)
{
        LASSERT (peer->ibp_persistence == 0);
        LASSERT (list_empty(&peer->ibp_conns));

        LASSERT (kibnal_peer_active(peer));
        list_del_init (&peer->ibp_list);
        /* lose peerlist's ref */
        kibnal_peer_decref(peer);
}

int
kibnal_get_peer_info (int index, ptl_nid_t *nidp, __u32 *ipp,
                      int *persistencep)
{
        kib_peer_t        *peer;
        struct list_head  *ptmp;
        int                i;
        unsigned long      flags;

        read_lock_irqsave(&kibnal_data.kib_global_lock, flags);

        for (i = 0; i < kibnal_data.kib_peer_hash_size; i++) {

                list_for_each (ptmp, &kibnal_data.kib_peers[i]) {

                        peer = list_entry (ptmp, kib_peer_t, ibp_list);
                        LASSERT (peer->ibp_persistence != 0 ||
                                 peer->ibp_connecting != 0 ||
                                 !list_empty (&peer->ibp_conns));

                        if (index-- > 0)
                                continue;

                        *nidp = peer->ibp_nid;
                        *ipp = peer->ibp_ip;
                        *persistencep = peer->ibp_persistence;

                        read_unlock_irqrestore(&kibnal_data.kib_global_lock,
                                               flags);
                        return (0);
                }
        }

        read_unlock_irqrestore(&kibnal_data.kib_global_lock, flags);
        return (-ENOENT);
}

int
kibnal_add_persistent_peer (ptl_nid_t nid, __u32 ip)
{
        kib_peer_t        *peer;
        kib_peer_t        *peer2;
        unsigned long      flags;

        CDEBUG(D_NET, LPX64"@%08x\n", nid, ip);
        
        if (nid == PTL_NID_ANY)
                return (-EINVAL);

        peer = kibnal_create_peer (nid);
        if (peer == NULL)
                return (-ENOMEM);

        write_lock_irqsave(&kibnal_data.kib_global_lock, flags);

        peer2 = kibnal_find_peer_locked (nid);
        if (peer2 != NULL) {
                kibnal_peer_decref (peer);
                peer = peer2;
        } else {
                /* peer table takes existing ref on peer */
                list_add_tail (&peer->ibp_list,
                               kibnal_nid2peerlist (nid));
        }

        peer->ibp_ip = ip;
        peer->ibp_persistence++;
        
        write_unlock_irqrestore(&kibnal_data.kib_global_lock, flags);
        return (0);
}

void
kibnal_del_peer_locked (kib_peer_t *peer, int single_share)
{
        struct list_head *ctmp;
        struct list_head *cnxt;
        kib_conn_t       *conn;

        if (!single_share)
                peer->ibp_persistence = 0;
        else if (peer->ibp_persistence > 0)
                peer->ibp_persistence--;

        if (peer->ibp_persistence != 0)
                return;

        if (list_empty(&peer->ibp_conns)) {
                kibnal_unlink_peer_locked(peer);
        } else {
                list_for_each_safe (ctmp, cnxt, &peer->ibp_conns) {
                        conn = list_entry(ctmp, kib_conn_t, ibc_list);

                        kibnal_close_conn_locked (conn, 0);
                }
                /* NB peer is no longer persistent; closing its last conn
                 * unlinked it. */
        }
        /* NB peer now unlinked; might even be freed if the peer table had the
         * last ref on it. */
}

int
kibnal_del_peer (ptl_nid_t nid, int single_share)
{
        struct list_head  *ptmp;
        struct list_head  *pnxt;
        kib_peer_t        *peer;
        int                lo;
        int                hi;
        int                i;
        unsigned long      flags;
        int                rc = -ENOENT;

        write_lock_irqsave(&kibnal_data.kib_global_lock, flags);

        if (nid != PTL_NID_ANY)
                lo = hi = kibnal_nid2peerlist(nid) - kibnal_data.kib_peers;
        else {
                lo = 0;
                hi = kibnal_data.kib_peer_hash_size - 1;
        }

        for (i = lo; i <= hi; i++) {
                list_for_each_safe (ptmp, pnxt, &kibnal_data.kib_peers[i]) {
                        peer = list_entry (ptmp, kib_peer_t, ibp_list);
                        LASSERT (peer->ibp_persistence != 0 ||
                                 peer->ibp_connecting != 0 ||
                                 !list_empty (&peer->ibp_conns));

                        if (!(nid == PTL_NID_ANY || peer->ibp_nid == nid))
                                continue;

                        kibnal_del_peer_locked (peer, single_share);
                        rc = 0;         /* matched something */

                        if (single_share)
                                goto out;
                }
        }
 out:
        write_unlock_irqrestore(&kibnal_data.kib_global_lock, flags);
        return (rc);
}

kib_conn_t *
kibnal_get_conn_by_idx (int index)
{
        kib_peer_t        *peer;
        struct list_head  *ptmp;
        kib_conn_t        *conn;
        struct list_head  *ctmp;
        int                i;
        unsigned long      flags;

        read_lock_irqsave(&kibnal_data.kib_global_lock, flags);

        for (i = 0; i < kibnal_data.kib_peer_hash_size; i++) {
                list_for_each (ptmp, &kibnal_data.kib_peers[i]) {

                        peer = list_entry (ptmp, kib_peer_t, ibp_list);
                        LASSERT (peer->ibp_persistence > 0 ||
                                 peer->ibp_connecting != 0 ||
                                 !list_empty (&peer->ibp_conns));

                        list_for_each (ctmp, &peer->ibp_conns) {
                                if (index-- > 0)
                                        continue;

                                conn = list_entry (ctmp, kib_conn_t, ibc_list);
                                kibnal_conn_addref(conn);
                                read_unlock_irqrestore(&kibnal_data.kib_global_lock,
                                                       flags);
                                return (conn);
                        }
                }
        }

        read_unlock_irqrestore(&kibnal_data.kib_global_lock, flags);
        return (NULL);
}

int
kibnal_set_qp_state (kib_conn_t *conn, vv_qp_state_t new_state)
{
        static vv_qp_attr_t attr;
        
        kib_connvars_t   *cv = conn->ibc_connvars;
        vv_return_t       vvrc;
        
        /* Only called by connd => static OK */
        LASSERT (!in_interrupt());
        LASSERT (current == kibnal_data.kib_connd);

        memset(&attr, 0, sizeof(attr));
        
        switch (new_state) {
        default:
                LBUG();
                
        case vv_qp_state_init: {
                struct vv_qp_modify_init_st *init = &attr.modify.params.init;

                init->p_key_indx     = cv->cv_pkey_index;
                init->phy_port_num   = cv->cv_port;
                init->q_key          = IBNAL_QKEY; /* XXX but VV_QP_AT_Q_KEY not set! */
                init->access_control = vv_acc_r_mem_read |
                                       vv_acc_r_mem_write; /* XXX vv_acc_l_mem_write ? */

                attr.modify.vv_qp_attr_mask = VV_QP_AT_P_KEY_IX | 
                                              VV_QP_AT_PHY_PORT_NUM |
                                              VV_QP_AT_ACCESS_CON_F;
                break;
        }
        case vv_qp_state_rtr: {
                struct vv_qp_modify_rtr_st *rtr = &attr.modify.params.rtr;
                vv_add_vec_t               *av  = &rtr->remote_add_vec;

                av->dlid                      = cv->cv_path.dlid;
                av->grh_flag                  = (!IBNAL_LOCAL_SUB);
                av->max_static_rate           = IBNAL_R_2_STATIC_RATE(cv->cv_path.rate);
                av->service_level             = cv->cv_path.sl;
                av->source_path_bit           = IBNAL_SOURCE_PATH_BIT;
                av->pmtu                      = cv->cv_path.mtu;
                av->rnr_retry_count           = cv->cv_rnr_count;
                av->global_dest.traffic_class = cv->cv_path.traffic_class;
                av->global_dest.hope_limit    = cv->cv_path.hop_limut;
                av->global_dest.flow_lable    = cv->cv_path.flow_label;
                av->global_dest.s_gid_index   = cv->cv_sgid_index;
                // XXX other av fields zero?

                rtr->destanation_qp            = cv->cv_remote_qpn;
                rtr->receive_psn               = cv->cv_rxpsn;
                rtr->responder_rdma_r_atom_num = IBNAL_OUS_DST_RD;

                // XXX ? rtr->opt_min_rnr_nak_timer = 16;


                // XXX sdp sets VV_QP_AT_OP_F but no actual optional options
                attr.modify.vv_qp_attr_mask = VV_QP_AT_ADD_VEC | 
                                              VV_QP_AT_DEST_QP |
                                              VV_QP_AT_R_PSN | 
                                              VV_QP_AT_MIN_RNR_NAK_T |
                                              VV_QP_AT_RESP_RDMA_ATOM_OUT_NUM |
                                              VV_QP_AT_OP_F;
                break;
        }
        case vv_qp_state_rts: {
                struct vv_qp_modify_rts_st *rts = &attr.modify.params.rts;

                rts->send_psn                 = cv->cv_txpsn;
                rts->local_ack_timeout        = IBNAL_LOCAL_ACK_TIMEOUT;
                rts->retry_num                = IBNAL_RETRY_CNT;
                rts->rnr_num                  = IBNAL_RNR_CNT;
                rts->dest_out_rdma_r_atom_num = IBNAL_OUS_DST_RD;
                
                attr.modify.vv_qp_attr_mask = VV_QP_AT_S_PSN |
                                              VV_QP_AT_L_ACK_T |
                                              VV_QP_AT_RETRY_NUM |
                                              VV_QP_AT_RNR_NUM |
                                              VV_QP_AT_DEST_RDMA_ATOM_OUT_NUM;
                break;
        }
        case vv_qp_state_error:
        case vv_qp_state_reset:
                attr.modify.vv_qp_attr_mask = 0;
                break;
        }
                
        attr.modify.qp_modify_into_state = new_state;
        attr.modify.vv_qp_attr_mask |= VV_QP_AT_STATE;
        
        vvrc = vv_qp_modify(kibnal_data.kib_hca, conn->ibc_qp, &attr, NULL);
        if (vvrc != vv_return_ok) {
                CERROR("Can't modify qp -> "LPX64" state to %d: %d\n", 
                       conn->ibc_peer->ibp_nid, new_state, vvrc);
                return -EIO;
        }
        
        return 0;
}

kib_conn_t *
kibnal_create_conn (cm_cep_handle_t cep)
{
        kib_conn_t   *conn;
        int           i;
        __u64         vaddr = 0;
        __u64         vaddr_base;
        int           page_offset;
        int           ipage;
        vv_return_t   vvrc;
        int           rc;

        static vv_qp_attr_t  reqattr;
        static vv_qp_attr_t  rspattr;

        /* Only the connd creates conns => single threaded */
        LASSERT(!in_interrupt());
        LASSERT(current == kibnal_data.kib_connd);
        
        PORTAL_ALLOC(conn, sizeof (*conn));
        if (conn == NULL) {
                CERROR ("Can't allocate connection\n");
                return (NULL);
        }

        /* zero flags, NULL pointers etc... */
        memset (conn, 0, sizeof (*conn));

        INIT_LIST_HEAD (&conn->ibc_early_rxs);
        INIT_LIST_HEAD (&conn->ibc_tx_queue);
        INIT_LIST_HEAD (&conn->ibc_active_txs);
        spin_lock_init (&conn->ibc_lock);
        
        atomic_inc (&kibnal_data.kib_nconns);
        /* well not really, but I call destroy() on failure, which decrements */

        conn->ibc_cep = cep;

        PORTAL_ALLOC(conn->ibc_connvars, sizeof(*conn->ibc_connvars));
        if (conn->ibc_connvars == NULL) {
                CERROR("Can't allocate in-progress connection state\n");
                goto failed;
        }
        memset (conn->ibc_connvars, 0, sizeof(*conn->ibc_connvars));
        /* Random seed for QP sequence number */
        get_random_bytes(&conn->ibc_connvars->cv_rxpsn,
                         sizeof(conn->ibc_connvars->cv_rxpsn));

        PORTAL_ALLOC(conn->ibc_rxs, IBNAL_RX_MSGS * sizeof (kib_rx_t));
        if (conn->ibc_rxs == NULL) {
                CERROR("Cannot allocate RX buffers\n");
                goto failed;
        }
        memset (conn->ibc_rxs, 0, IBNAL_RX_MSGS * sizeof(kib_rx_t));

        rc = kibnal_alloc_pages(&conn->ibc_rx_pages, IBNAL_RX_MSG_PAGES, 1);
        if (rc != 0)
                goto failed;

        vaddr_base = vaddr = conn->ibc_rx_pages->ibp_vaddr;

        for (i = ipage = page_offset = 0; i < IBNAL_RX_MSGS; i++) {
                struct page *page = conn->ibc_rx_pages->ibp_pages[ipage];
                kib_rx_t   *rx = &conn->ibc_rxs[i];

                rx->rx_conn = conn;
                rx->rx_msg = (kib_msg_t *)(((char *)page_address(page)) + 
                             page_offset);

#if IBNAL_WHOLE_MEM
                {
                        vv_mem_reg_h_t  mem_h;
                        vv_r_key_t      r_key;

                        /* Voltaire stack already registers the whole
                         * memory, so use that API. */
                        vvrc = vv_get_gen_mr_attrib(kibnal_data.kib_hca,
                                                    rx->rx_msg,
                                                    IBNAL_MSG_SIZE,
                                                    &mem_h,
                                                    &rx->rx_lkey,
                                                    &r_key);
                        LASSERT (vvrc == vv_return_ok);
                }
#else
                rx->rx_vaddr = vaddr;
#endif                
                CDEBUG(D_NET, "Rx[%d] %p->%p[%x:"LPX64"]\n", i, rx, 
                       rx->rx_msg, KIBNAL_RX_LKEY(rx), KIBNAL_RX_VADDR(rx));

                vaddr += IBNAL_MSG_SIZE;
                LASSERT (vaddr <= vaddr_base + IBNAL_RX_MSG_BYTES);
                
                page_offset += IBNAL_MSG_SIZE;
                LASSERT (page_offset <= PAGE_SIZE);

                if (page_offset == PAGE_SIZE) {
                        page_offset = 0;
                        ipage++;
                        LASSERT (ipage <= IBNAL_RX_MSG_PAGES);
                }
        }

        memset(&reqattr, 0, sizeof(reqattr));

        reqattr.create.qp_type                    = vv_qp_type_r_conn;
        reqattr.create.cq_send_h                  = kibnal_data.kib_cq;
        reqattr.create.cq_receive_h               = kibnal_data.kib_cq;
        reqattr.create.send_max_outstand_wr       = (1 + IBNAL_MAX_RDMA_FRAGS) * 
                                                    IBNAL_MSG_QUEUE_SIZE;
        reqattr.create.receive_max_outstand_wr    = IBNAL_RX_MSGS;
        reqattr.create.max_scatgat_per_send_wr    = 1;
        reqattr.create.max_scatgat_per_receive_wr = 1;
        reqattr.create.signaling_type             = vv_selectable_signaling;
        reqattr.create.pd_h                       = kibnal_data.kib_pd;
        reqattr.create.recv_solicited_events      = vv_selectable_signaling; // vv_signal_all;

        vvrc = vv_qp_create(kibnal_data.kib_hca, &reqattr, NULL,
                            &conn->ibc_qp, &rspattr);
        if (vvrc != vv_return_ok) {
                CERROR ("Failed to create queue pair: %d\n", vvrc);
                goto failed;
        }

        /* Mark QP created */
        conn->ibc_state = IBNAL_CONN_INIT;
        conn->ibc_connvars->cv_local_qpn = rspattr.create_return.qp_num;

        if (rspattr.create_return.receive_max_outstand_wr < 
            IBNAL_MSG_QUEUE_SIZE ||
            rspattr.create_return.send_max_outstand_wr < 
            (1 + IBNAL_MAX_RDMA_FRAGS) * IBNAL_MSG_QUEUE_SIZE) {
                CERROR("Insufficient rx/tx work items: wanted %d/%d got %d/%d\n",
                       IBNAL_MSG_QUEUE_SIZE, 
                       (1 + IBNAL_MAX_RDMA_FRAGS) * IBNAL_MSG_QUEUE_SIZE,
                       rspattr.create_return.receive_max_outstand_wr,
                       rspattr.create_return.send_max_outstand_wr);
                goto failed;
        }

        /* 1 ref for caller */
        atomic_set (&conn->ibc_refcount, 1);
        return (conn);
        
 failed:
        kibnal_destroy_conn (conn);
        return (NULL);
}

void
kibnal_destroy_conn (kib_conn_t *conn)
{
        vv_return_t vvrc;

        /* Only the connd does this (i.e. single threaded) */
        LASSERT (!in_interrupt());
        LASSERT (current == kibnal_data.kib_connd);
        
        CDEBUG (D_NET, "connection %p\n", conn);

        LASSERT (atomic_read (&conn->ibc_refcount) == 0);
        LASSERT (list_empty(&conn->ibc_early_rxs));
        LASSERT (list_empty(&conn->ibc_tx_queue));
        LASSERT (list_empty(&conn->ibc_active_txs));
        LASSERT (conn->ibc_nsends_posted == 0);

        switch (conn->ibc_state) {
        default:
                /* conn must be completely disengaged from the network */
                LBUG();

        case IBNAL_CONN_DISCONNECTED:
                /* connvars should have been freed already */
                LASSERT (conn->ibc_connvars == NULL);
                /* fall through */

        case IBNAL_CONN_INIT:
                kibnal_set_qp_state(conn, vv_qp_state_reset);
                vvrc = vv_qp_destroy(kibnal_data.kib_hca, conn->ibc_qp);
                if (vvrc != vv_return_ok)
                        CERROR("Can't destroy QP: %d\n", vvrc);
                /* fall through */
                
        case IBNAL_CONN_INIT_NOTHING:
                break;
        }

        if (conn->ibc_rx_pages != NULL) 
                kibnal_free_pages(conn->ibc_rx_pages);

        if (conn->ibc_rxs != NULL)
                PORTAL_FREE(conn->ibc_rxs, 
                            IBNAL_RX_MSGS * sizeof(kib_rx_t));

        if (conn->ibc_connvars != NULL)
                PORTAL_FREE(conn->ibc_connvars, sizeof(*conn->ibc_connvars));

        if (conn->ibc_peer != NULL)
                kibnal_peer_decref(conn->ibc_peer);

        vvrc = cm_destroy_cep(conn->ibc_cep);
        LASSERT (vvrc == vv_return_ok);

        PORTAL_FREE(conn, sizeof (*conn));

        atomic_dec(&kibnal_data.kib_nconns);
}

int
kibnal_close_peer_conns_locked (kib_peer_t *peer, int why)
{
        kib_conn_t         *conn;
        struct list_head   *ctmp;
        struct list_head   *cnxt;
        int                 count = 0;

        list_for_each_safe (ctmp, cnxt, &peer->ibp_conns) {
                conn = list_entry (ctmp, kib_conn_t, ibc_list);

                count++;
                kibnal_close_conn_locked (conn, why);
        }

        return (count);
}

int
kibnal_close_stale_conns_locked (kib_peer_t *peer, __u64 incarnation)
{
        kib_conn_t         *conn;
        struct list_head   *ctmp;
        struct list_head   *cnxt;
        int                 count = 0;

        list_for_each_safe (ctmp, cnxt, &peer->ibp_conns) {
                conn = list_entry (ctmp, kib_conn_t, ibc_list);

                if (conn->ibc_incarnation == incarnation)
                        continue;

                CDEBUG(D_NET, "Closing stale conn nid:"LPX64" incarnation:"LPX64"("LPX64")\n",
                       peer->ibp_nid, conn->ibc_incarnation, incarnation);
                
                count++;
                kibnal_close_conn_locked (conn, -ESTALE);
        }

        return (count);
}

int
kibnal_close_matching_conns (ptl_nid_t nid)
{
        kib_peer_t         *peer;
        struct list_head   *ptmp;
        struct list_head   *pnxt;
        int                 lo;
        int                 hi;
        int                 i;
        unsigned long       flags;
        int                 count = 0;

        write_lock_irqsave(&kibnal_data.kib_global_lock, flags);

        if (nid != PTL_NID_ANY)
                lo = hi = kibnal_nid2peerlist(nid) - kibnal_data.kib_peers;
        else {
                lo = 0;
                hi = kibnal_data.kib_peer_hash_size - 1;
        }

        for (i = lo; i <= hi; i++) {
                list_for_each_safe (ptmp, pnxt, &kibnal_data.kib_peers[i]) {

                        peer = list_entry (ptmp, kib_peer_t, ibp_list);
                        LASSERT (peer->ibp_persistence != 0 ||
                                 peer->ibp_connecting != 0 ||
                                 !list_empty (&peer->ibp_conns));

                        if (!(nid == PTL_NID_ANY || nid == peer->ibp_nid))
                                continue;

                        count += kibnal_close_peer_conns_locked (peer, 0);
                }
        }

        write_unlock_irqrestore(&kibnal_data.kib_global_lock, flags);

        /* wildcards always succeed */
        if (nid == PTL_NID_ANY)
                return (0);
        
        return (count == 0 ? -ENOENT : 0);
}

int
kibnal_cmd(struct portals_cfg *pcfg, void * private)
{
        int rc = -EINVAL;

        LASSERT (pcfg != NULL);

        switch(pcfg->pcfg_command) {
        case NAL_CMD_GET_PEER: {
                ptl_nid_t   nid = 0;
                __u32       ip = 0;
                int         share_count = 0;

                rc = kibnal_get_peer_info(pcfg->pcfg_count,
                                          &nid, &ip, &share_count);
                pcfg->pcfg_nid   = nid;
                pcfg->pcfg_size  = 0;
                pcfg->pcfg_id    = ip;
                pcfg->pcfg_misc  = IBNAL_SERVICE_NUMBER; /* port */
                pcfg->pcfg_count = 0;
                pcfg->pcfg_wait  = share_count;
                break;
        }
        case NAL_CMD_ADD_PEER: {
                rc = kibnal_add_persistent_peer (pcfg->pcfg_nid,
                                                 pcfg->pcfg_id); /* IP */
                break;
        }
        case NAL_CMD_DEL_PEER: {
                rc = kibnal_del_peer (pcfg->pcfg_nid, 
                                       /* flags == single_share */
                                       pcfg->pcfg_flags != 0);
                break;
        }
        case NAL_CMD_GET_CONN: {
                kib_conn_t *conn = kibnal_get_conn_by_idx (pcfg->pcfg_count);

                if (conn == NULL)
                        rc = -ENOENT;
                else {
                        rc = 0;
                        pcfg->pcfg_nid   = conn->ibc_peer->ibp_nid;
                        pcfg->pcfg_id    = 0;
                        pcfg->pcfg_misc  = 0;
                        pcfg->pcfg_flags = 0;
                        kibnal_conn_decref(conn);
                }
                break;
        }
        case NAL_CMD_CLOSE_CONNECTION: {
                rc = kibnal_close_matching_conns (pcfg->pcfg_nid);
                break;
        }
        case NAL_CMD_REGISTER_MYNID: {
                if (pcfg->pcfg_nid == PTL_NID_ANY)
                        rc = -EINVAL;
                else
                        rc = kibnal_set_mynid (pcfg->pcfg_nid);
                break;
        }
        }

        return rc;
}

void
kibnal_free_pages (kib_pages_t *p)
{
        int         npages = p->ibp_npages;
        vv_return_t vvrc;
        int         i;
        
        if (p->ibp_mapped) {
                vvrc = vv_mem_region_destroy(kibnal_data.kib_hca, 
                                             p->ibp_handle);
                if (vvrc != vv_return_ok)
                        CERROR ("Deregister error: %d\n", vvrc);
        }
        
        for (i = 0; i < npages; i++)
                if (p->ibp_pages[i] != NULL)
                        __free_page(p->ibp_pages[i]);
        
        PORTAL_FREE (p, offsetof(kib_pages_t, ibp_pages[npages]));
}

int
kibnal_alloc_pages (kib_pages_t **pp, int npages, int allow_write)
{
        kib_pages_t   *p;
        int            i;
#if !IBNAL_WHOLE_MEM
        vv_phy_list_t            vv_phys;
        vv_phy_buf_t            *phys_pages;
        vv_return_t              vvrc;
        vv_access_con_bit_mask_t access;
#endif

        PORTAL_ALLOC(p, offsetof(kib_pages_t, ibp_pages[npages]));
        if (p == NULL) {
                CERROR ("Can't allocate buffer %d\n", npages);
                return (-ENOMEM);
        }

        memset (p, 0, offsetof(kib_pages_t, ibp_pages[npages]));
        p->ibp_npages = npages;
        
        for (i = 0; i < npages; i++) {
                p->ibp_pages[i] = alloc_page (GFP_KERNEL);
                if (p->ibp_pages[i] == NULL) {
                        CERROR ("Can't allocate page %d of %d\n", i, npages);
                        kibnal_free_pages(p);
                        return (-ENOMEM);
                }
        }

#if !IBNAL_WHOLE_MEM
        PORTAL_ALLOC(phys_pages, npages * sizeof(*phys_pages));
        if (phys_pages == NULL) {
                CERROR ("Can't allocate physarray for %d pages\n", npages);
                kibnal_free_pages(p);
                return (-ENOMEM);
        }

        vv_phys.number_of_buff = npages;
        vv_phys.phy_list = phys_pages;

        for (i = 0; i < npages; i++) {
                phys_pages[i].size = PAGE_SIZE;
                phys_pages[i].start = page_to_phys(p->ibp_pages[i]);
        }

        VV_ACCESS_CONTROL_MASK_SET_ALL(access);
        
        vvrc = vv_phy_mem_region_register(kibnal_data.kib_hca,
                                          &vv_phys,
                                          0, /* requested vaddr */
                                          npages * PAGE_SIZE, 0, /* offset */
                                          kibnal_data.kib_pd,
                                          access,
                                          &p->ibp_handle, 
                                          &p->ibp_vaddr,                                           
                                          &p->ibp_lkey, 
                                          &p->ibp_rkey);
        
        PORTAL_FREE(phys_pages, npages * sizeof(*phys_pages));
        
        if (vvrc != vv_return_ok) {
                CERROR ("Error %d mapping %d pages\n", vvrc, npages);
                kibnal_free_pages(p);
                return (-EFAULT);
        }

        CDEBUG(D_NET, "registered %d pages; handle: %x vaddr "LPX64" "
               "lkey %x rkey %x\n", npages, p->ibp_handle,
               p->ibp_vaddr, p->ibp_lkey, p->ibp_rkey);
        
        p->ibp_mapped = 1;
#endif
        *pp = p;
        return (0);
}

int
kibnal_alloc_tx_descs (void) 
{
        int    i;
        
        PORTAL_ALLOC (kibnal_data.kib_tx_descs,
                      IBNAL_TX_MSGS * sizeof(kib_tx_t));
        if (kibnal_data.kib_tx_descs == NULL)
                return -ENOMEM;
        
        memset(kibnal_data.kib_tx_descs, 0,
               IBNAL_TX_MSGS * sizeof(kib_tx_t));

        for (i = 0; i < IBNAL_TX_MSGS; i++) {
                kib_tx_t *tx = &kibnal_data.kib_tx_descs[i];

                PORTAL_ALLOC(tx->tx_wrq, 
                             (1 + IBNAL_MAX_RDMA_FRAGS) * 
                             sizeof(*tx->tx_wrq));
                if (tx->tx_wrq == NULL)
                        return -ENOMEM;
                
                PORTAL_ALLOC(tx->tx_gl, 
                             (1 + IBNAL_MAX_RDMA_FRAGS) * 
                             sizeof(*tx->tx_gl));
                if (tx->tx_gl == NULL)
                        return -ENOMEM;
                
                PORTAL_ALLOC(tx->tx_rd, 
                             offsetof(kib_rdma_desc_t, 
                                      rd_frags[IBNAL_MAX_RDMA_FRAGS]));
                if (tx->tx_rd == NULL)
                        return -ENOMEM;
        }

        return 0;
}

void
kibnal_free_tx_descs (void) 
{
        int    i;

        if (kibnal_data.kib_tx_descs == NULL)
                return;

        for (i = 0; i < IBNAL_TX_MSGS; i++) {
                kib_tx_t *tx = &kibnal_data.kib_tx_descs[i];

                if (tx->tx_wrq != NULL)
                        PORTAL_FREE(tx->tx_wrq, 
                                    (1 + IBNAL_MAX_RDMA_FRAGS) * 
                                    sizeof(*tx->tx_wrq));

                if (tx->tx_gl != NULL)
                        PORTAL_FREE(tx->tx_gl, 
                                    (1 + IBNAL_MAX_RDMA_FRAGS) * 
                                    sizeof(*tx->tx_gl));

                if (tx->tx_rd != NULL)
                        PORTAL_FREE(tx->tx_rd, 
                                    offsetof(kib_rdma_desc_t, 
                                             rd_frags[IBNAL_MAX_RDMA_FRAGS]));
        }

        PORTAL_FREE(kibnal_data.kib_tx_descs,
                    IBNAL_TX_MSGS * sizeof(kib_tx_t));
}

int
kibnal_setup_tx_descs (void)
{
        int           ipage = 0;
        int           page_offset = 0;
        __u64         vaddr;
        __u64         vaddr_base;
        struct page  *page;
        kib_tx_t     *tx;
        int           i;
        int           rc;

        /* pre-mapped messages are not bigger than 1 page */
        CLASSERT (IBNAL_MSG_SIZE <= PAGE_SIZE);

        /* No fancy arithmetic when we do the buffer calculations */
        CLASSERT (PAGE_SIZE % IBNAL_MSG_SIZE == 0);

        rc = kibnal_alloc_pages(&kibnal_data.kib_tx_pages, IBNAL_TX_MSG_PAGES, 
                                0);
        if (rc != 0)
                return (rc);

        /* ignored for the whole_mem case */
        vaddr = vaddr_base = kibnal_data.kib_tx_pages->ibp_vaddr;

        for (i = 0; i < IBNAL_TX_MSGS; i++) {
                page = kibnal_data.kib_tx_pages->ibp_pages[ipage];
                tx = &kibnal_data.kib_tx_descs[i];

                tx->tx_msg = (kib_msg_t *)(((char *)page_address(page)) + 
                                           page_offset);
#if IBNAL_WHOLE_MEM
                {
                        vv_mem_reg_h_t  mem_h;
                        vv_r_key_t      rkey;
                        vv_return_t     vvrc;

                        /* Voltaire stack already registers the whole
                         * memory, so use that API. */
                        vvrc = vv_get_gen_mr_attrib(kibnal_data.kib_hca,
                                                    tx->tx_msg,
                                                    IBNAL_MSG_SIZE,
                                                    &mem_h,
                                                    &tx->tx_lkey,
                                                    &rkey);
                        LASSERT (vvrc == vv_return_ok);
                }
#else
                tx->tx_vaddr = vaddr;
#endif
                tx->tx_isnblk = (i >= IBNAL_NTX);
                tx->tx_mapped = KIB_TX_UNMAPPED;

                CDEBUG(D_NET, "Tx[%d] %p->%p[%x:"LPX64"]\n", i, tx, 
                       tx->tx_msg, KIBNAL_TX_LKEY(tx), KIBNAL_TX_VADDR(tx));

                if (tx->tx_isnblk)
                        list_add (&tx->tx_list, 
                                  &kibnal_data.kib_idle_nblk_txs);
                else
                        list_add (&tx->tx_list, 
                                  &kibnal_data.kib_idle_txs);

                vaddr += IBNAL_MSG_SIZE;
                LASSERT (vaddr <= vaddr_base + IBNAL_TX_MSG_BYTES);

                page_offset += IBNAL_MSG_SIZE;
                LASSERT (page_offset <= PAGE_SIZE);

                if (page_offset == PAGE_SIZE) {
                        page_offset = 0;
                        ipage++;
                        LASSERT (ipage <= IBNAL_TX_MSG_PAGES);
                }
        }
        
        return (0);
}

void
kibnal_api_shutdown (nal_t *nal)
{
        int         i;
        vv_return_t vvrc;

        if (nal->nal_refct != 0) {
                /* This module got the first ref */
                PORTAL_MODULE_UNUSE;
                return;
        }

        CDEBUG(D_MALLOC, "before NAL cleanup: kmem %d\n",
               atomic_read (&portal_kmemory));

        LASSERT(nal == &kibnal_api);

        switch (kibnal_data.kib_init) {

        case IBNAL_INIT_ALL:
                /* stop calls to nal_cmd */
                libcfs_nal_cmd_unregister(VIBNAL);
                /* No new peers */

                /* resetting my NID removes my listener and nukes all current
                 * peers and their connections */
                kibnal_set_mynid (PTL_NID_ANY);

                /* Wait for all peer state to clean up */
                i = 2;
                while (atomic_read (&kibnal_data.kib_npeers) != 0) {
                        i++;
                        CDEBUG(((i & (-i)) == i) ? D_WARNING : D_NET, /* power of 2? */
                               "waiting for %d peers to disconnect\n",
                               atomic_read (&kibnal_data.kib_npeers));
                        set_current_state (TASK_UNINTERRUPTIBLE);
                        schedule_timeout (HZ);
                }
                /* fall through */

        case IBNAL_INIT_CQ:
                vvrc = vv_cq_destroy(kibnal_data.kib_hca, kibnal_data.kib_cq);
                if (vvrc != vv_return_ok)
                        CERROR ("Destroy CQ error: %d\n", vvrc);
                /* fall through */

        case IBNAL_INIT_TXD:
                kibnal_free_pages (kibnal_data.kib_tx_pages);
                /* fall through */

        case IBNAL_INIT_PD:
#if !IBNAL_WHOLE_MEM
                vvrc = vv_pd_deallocate(kibnal_data.kib_hca,
                                        kibnal_data.kib_pd);
                if (vvrc != vv_return_ok)
                        CERROR ("Destroy PD error: %d\n", vvrc);
#endif
                /* fall through */

        case IBNAL_INIT_ASYNC:
                vvrc = vv_dell_async_event_cb (kibnal_data.kib_hca,
                                              kibnal_async_callback);
                if (vvrc != vv_return_ok)
                        CERROR("vv_dell_async_event_cb error: %d\n", vvrc);
                        
                /* fall through */

        case IBNAL_INIT_HCA:
                vvrc = vv_hca_close(kibnal_data.kib_hca);
                if (vvrc != vv_return_ok)
                        CERROR ("Close HCA  error: %d\n", vvrc);
                /* fall through */

        case IBNAL_INIT_LIB:
                lib_fini(&kibnal_lib);
                /* fall through */

        case IBNAL_INIT_DATA:
                LASSERT (atomic_read (&kibnal_data.kib_npeers) == 0);
                LASSERT (kibnal_data.kib_peers != NULL);
                for (i = 0; i < kibnal_data.kib_peer_hash_size; i++) {
                        LASSERT (list_empty (&kibnal_data.kib_peers[i]));
                }
                LASSERT (atomic_read (&kibnal_data.kib_nconns) == 0);
                LASSERT (list_empty (&kibnal_data.kib_sched_rxq));
                LASSERT (list_empty (&kibnal_data.kib_sched_txq));
                LASSERT (list_empty (&kibnal_data.kib_connd_zombies));
                LASSERT (list_empty (&kibnal_data.kib_connd_conns));
                LASSERT (list_empty (&kibnal_data.kib_connd_pcreqs));
                LASSERT (list_empty (&kibnal_data.kib_connd_peers));

                /* flag threads to terminate; wake and wait for them to die */
                kibnal_data.kib_shutdown = 1;
                wake_up_all (&kibnal_data.kib_sched_waitq);
                wake_up_all (&kibnal_data.kib_connd_waitq);

                i = 2;
                while (atomic_read (&kibnal_data.kib_nthreads) != 0) {
                        i++;
                        CDEBUG(((i & (-i)) == i) ? D_WARNING : D_NET, /* power of 2? */
                               "Waiting for %d threads to terminate\n",
                               atomic_read (&kibnal_data.kib_nthreads));
                        set_current_state (TASK_INTERRUPTIBLE);
                        schedule_timeout (HZ);
                }
                /* fall through */
                
        case IBNAL_INIT_NOTHING:
                break;
        }

        kibnal_free_tx_descs();

        if (kibnal_data.kib_peers != NULL)
                PORTAL_FREE (kibnal_data.kib_peers,
                             sizeof (struct list_head) * 
                             kibnal_data.kib_peer_hash_size);

        CDEBUG(D_MALLOC, "after NAL cleanup: kmem %d\n",
               atomic_read (&portal_kmemory));
        printk(KERN_INFO "Lustre: Voltaire IB NAL unloaded (final mem %d)\n",
               atomic_read(&portal_kmemory));

        kibnal_data.kib_init = IBNAL_INIT_NOTHING;
}

int
kibnal_api_startup (nal_t *nal, ptl_pid_t requested_pid,
                     ptl_ni_limits_t *requested_limits,
                     ptl_ni_limits_t *actual_limits)
{
        struct timeval            tv;
        ptl_process_id_t          process_id;
        int                       pkmem = atomic_read(&portal_kmemory);
        int                       rc;
        int                       i;
        vv_request_event_record_t req_er;
        vv_return_t               vvrc;

        LASSERT (nal == &kibnal_api);

        if (nal->nal_refct != 0) {
                if (actual_limits != NULL)
                        *actual_limits = kibnal_lib.libnal_ni.ni_actual_limits;
                /* This module got the first ref */
                PORTAL_MODULE_USE;
                return (PTL_OK);
        }

        LASSERT (kibnal_data.kib_init == IBNAL_INIT_NOTHING);
        memset (&kibnal_data, 0, sizeof (kibnal_data)); /* zero pointers, flags etc */
        
        do_gettimeofday(&tv);
        kibnal_data.kib_incarnation = (((__u64)tv.tv_sec) * 1000000) + tv.tv_usec;
        kibnal_data.kib_svc_id = IBNAL_SERVICE_NUMBER;

        init_MUTEX (&kibnal_data.kib_nid_mutex);

        rwlock_init(&kibnal_data.kib_global_lock);

        kibnal_data.kib_peer_hash_size = IBNAL_PEER_HASH_SIZE;
        PORTAL_ALLOC (kibnal_data.kib_peers,
                      sizeof (struct list_head) * kibnal_data.kib_peer_hash_size);
        if (kibnal_data.kib_peers == NULL) {
                goto failed;
        }
        for (i = 0; i < kibnal_data.kib_peer_hash_size; i++)
                INIT_LIST_HEAD(&kibnal_data.kib_peers[i]);

        spin_lock_init (&kibnal_data.kib_connd_lock);
        INIT_LIST_HEAD (&kibnal_data.kib_connd_peers);
        INIT_LIST_HEAD (&kibnal_data.kib_connd_pcreqs);
        INIT_LIST_HEAD (&kibnal_data.kib_connd_conns);
        INIT_LIST_HEAD (&kibnal_data.kib_connd_zombies);
        init_waitqueue_head (&kibnal_data.kib_connd_waitq);

        spin_lock_init (&kibnal_data.kib_sched_lock);
        INIT_LIST_HEAD (&kibnal_data.kib_sched_txq);
        INIT_LIST_HEAD (&kibnal_data.kib_sched_rxq);
        init_waitqueue_head (&kibnal_data.kib_sched_waitq);

        spin_lock_init (&kibnal_data.kib_tx_lock);
        INIT_LIST_HEAD (&kibnal_data.kib_idle_txs);
        INIT_LIST_HEAD (&kibnal_data.kib_idle_nblk_txs);
        init_waitqueue_head(&kibnal_data.kib_idle_tx_waitq);

        rc = kibnal_alloc_tx_descs();
        if (rc != 0) {
                CERROR("Can't allocate tx descs\n");
                goto failed;
        }
        
        /* lists/ptrs/locks initialised */
        kibnal_data.kib_init = IBNAL_INIT_DATA;
        /*****************************************************/

        process_id.pid = requested_pid;
        process_id.nid = PTL_NID_ANY;
        
        rc = lib_init(&kibnal_lib, nal, process_id,
                      requested_limits, actual_limits);
        if (rc != PTL_OK) {
                CERROR("lib_init failed: error %d\n", rc);
                goto failed;
        }

        /* lib interface initialised */
        kibnal_data.kib_init = IBNAL_INIT_LIB;
        /*****************************************************/

        for (i = 0; i < IBNAL_N_SCHED; i++) {
                rc = kibnal_thread_start (kibnal_scheduler, (void *)((long)i));
                if (rc != 0) {
                        CERROR("Can't spawn vibnal scheduler[%d]: %d\n",
                               i, rc);
                        goto failed;
                }
        }

        rc = kibnal_thread_start (kibnal_connd, NULL);
        if (rc != 0) {
                CERROR ("Can't spawn vibnal connd: %d\n", rc);
                goto failed;
        }

        /* TODO: apparently only one adapter is supported */
        vvrc = vv_hca_open("ANY_HCA", NULL, &kibnal_data.kib_hca);
        if (vvrc != vv_return_ok) {
                CERROR ("Can't open CA: %d\n", vvrc);
                goto failed;
        }

        /* Channel Adapter opened */
        kibnal_data.kib_init = IBNAL_INIT_HCA;

        /* register to get HCA's asynchronous events. */
        req_er.req_event_type = VV_ASYNC_EVENT_ALL_MASK;
        vvrc = vv_set_async_event_cb (kibnal_data.kib_hca, req_er,
                                     kibnal_async_callback);
        if (vvrc != vv_return_ok) {
                CERROR ("Can't open CA: %d\n", vvrc);
                goto failed; 
        }

        kibnal_data.kib_init = IBNAL_INIT_ASYNC;

        /*****************************************************/

        vvrc = vv_hca_query(kibnal_data.kib_hca, &kibnal_data.kib_hca_attrs);
        if (vvrc != vv_return_ok) {
                CERROR ("Can't size port attrs: %d\n", vvrc);
                goto failed;
        }

        kibnal_data.kib_port = -1;

        for (i = 0; i<kibnal_data.kib_hca_attrs.port_num; i++) {

                int port_num = i+1;
                u_int32_t tbl_count;
                vv_port_attrib_t *pattr = &kibnal_data.kib_port_attr;

                vvrc = vv_port_query(kibnal_data.kib_hca, port_num, pattr);
                if (vvrc != vv_return_ok) {
                        CERROR("vv_port_query failed for port %d: %d\n",
                               port_num, vvrc);
                        continue;
                }

                switch (pattr->port_state) {
                case vv_state_linkDoun:
                        CDEBUG(D_NET, "port[%d] Down\n", port_num);
                        continue;
                case vv_state_linkInit:
                        CDEBUG(D_NET, "port[%d] Init\n", port_num);
                        continue;
                case vv_state_linkArm:
                        CDEBUG(D_NET, "port[%d] Armed\n", port_num);
                        continue;
                case vv_state_linkActive:
                        CDEBUG(D_NET, "port[%d] Active\n", port_num);

                        /* Found a suitable port. Get its GUID and PKEY. */
                        kibnal_data.kib_port = port_num;
                        
                        tbl_count = 1;
                        vvrc = vv_get_port_gid_tbl(kibnal_data.kib_hca, 
                                                   port_num, &tbl_count,
                                                   &kibnal_data.kib_port_gid);
                        if (vvrc != vv_return_ok) {
                                CERROR("vv_get_port_gid_tbl failed "
                                       "for port %d: %d\n", port_num, vvrc);
                                continue;
                        }

                        tbl_count = 1;
                        vvrc = vv_get_port_partition_tbl(kibnal_data.kib_hca, 
                                                        port_num, &tbl_count,
                                                        &kibnal_data.kib_port_pkey);
                        if (vvrc != vv_return_ok) {
                                CERROR("vv_get_port_partition_tbl failed "
                                       "for port %d: %d\n", port_num, vvrc);
                                continue;
                        }

                        break;
                case vv_state_linkActDefer: /* TODO: correct? */
                case vv_state_linkNoChange:
                        CERROR("Unexpected port[%d] state %d\n",
                               i, pattr->port_state);
                        continue;
                }
                break;
        }

        if (kibnal_data.kib_port == -1) {
                CERROR ("Can't find an active port\n");
                goto failed;
        }

        CDEBUG(D_NET, "Using port %d - GID="LPX64":"LPX64"\n",
               kibnal_data.kib_port, 
               kibnal_data.kib_port_gid.scope.g.subnet, 
               kibnal_data.kib_port_gid.scope.g.eui64);
        
        /*****************************************************/

#if !IBNAL_WHOLE_MEM
        vvrc = vv_pd_allocate(kibnal_data.kib_hca, &kibnal_data.kib_pd);
#else
        vvrc = vv_get_gen_pd_h(kibnal_data.kib_hca, &kibnal_data.kib_pd);
#endif
        if (vvrc != 0) {
                CERROR ("Can't create PD: %d\n", vvrc);
                goto failed;
        }
        
        /* flag PD initialised */
        kibnal_data.kib_init = IBNAL_INIT_PD;
        /*****************************************************/

        rc = kibnal_setup_tx_descs();
        if (rc != 0) {
                CERROR ("Can't register tx descs: %d\n", rc);
                goto failed;
        }
        
        /* flag TX descs initialised */
        kibnal_data.kib_init = IBNAL_INIT_TXD;
        /*****************************************************/
        {
                uint32_t nentries;

                vvrc = vv_cq_create(kibnal_data.kib_hca, IBNAL_CQ_ENTRIES,
                                    kibnal_cq_callback, 
                                    NULL, /* context */
                                    &kibnal_data.kib_cq, &nentries);
                if (vvrc != 0) {
                        CERROR ("Can't create RX CQ: %d\n", vvrc);
                        goto failed;
                }

                /* flag CQ initialised */
                kibnal_data.kib_init = IBNAL_INIT_CQ;

                if (nentries < IBNAL_CQ_ENTRIES) {
                        CERROR ("CQ only has %d entries, need %d\n", 
                                nentries, IBNAL_CQ_ENTRIES);
                        goto failed;
                }

                vvrc = vv_request_completion_notification(kibnal_data.kib_hca, 
                                                          kibnal_data.kib_cq, 
                                                          vv_next_solicit_unsolicit_event);
                if (vvrc != 0) {
                        CERROR ("Failed to re-arm completion queue: %d\n", rc);
                        goto failed;
                }
        }
        
        /*****************************************************/

        rc = libcfs_nal_cmd_register(VIBNAL, &kibnal_cmd, NULL);
        if (rc != 0) {
                CERROR ("Can't initialise command interface (rc = %d)\n", rc);
                goto failed;
        }

        /* flag everything initialised */
        kibnal_data.kib_init = IBNAL_INIT_ALL;
        /*****************************************************/

        printk(KERN_INFO "Lustre: Voltaire IB NAL loaded "
               "(initial mem %d)\n", pkmem);

        return (PTL_OK);

 failed:
        CDEBUG(D_NET, "kibnal_api_startup failed\n");
        kibnal_api_shutdown (&kibnal_api);    
        return (PTL_FAIL);
}

void __exit
kibnal_module_fini (void)
{
#ifdef CONFIG_SYSCTL
        if (kibnal_tunables.kib_sysctl != NULL)
                unregister_sysctl_table (kibnal_tunables.kib_sysctl);
#endif
        PtlNIFini(kibnal_ni);

        ptl_unregister_nal(VIBNAL);
}

int __init
kibnal_module_init (void)
{
        int    rc;

        CLASSERT (offsetof(kib_msg_t, ibm_u) + sizeof(kib_connparams_t) 
                  <= cm_REQ_priv_data_len);
        CLASSERT (offsetof(kib_msg_t, ibm_u) + sizeof(kib_connparams_t) 
                  <= cm_REP_priv_data_len);
        CLASSERT (offsetof(kib_msg_t, ibm_u.get.ibgm_rd.rd_frags[IBNAL_MAX_RDMA_FRAGS])
                  <= IBNAL_MSG_SIZE);
        CLASSERT (offsetof(kib_msg_t, ibm_u.putack.ibpam_rd.rd_frags[IBNAL_MAX_RDMA_FRAGS])
                  <= IBNAL_MSG_SIZE);
        
        /* the following must be sizeof(int) for proc_dointvec() */
        CLASSERT (sizeof (kibnal_tunables.kib_io_timeout) == sizeof (int));

        kibnal_api.nal_ni_init = kibnal_api_startup;
        kibnal_api.nal_ni_fini = kibnal_api_shutdown;

        /* Initialise dynamic tunables to defaults once only */
        kibnal_tunables.kib_io_timeout = IBNAL_IO_TIMEOUT;

        rc = ptl_register_nal(VIBNAL, &kibnal_api);
        if (rc != PTL_OK) {
                CERROR("Can't register IBNAL: %d\n", rc);
                return (-ENOMEM);               /* or something... */
        }

        /* Pure gateways want the NAL started up at module load time... */
        rc = PtlNIInit(VIBNAL, LUSTRE_SRV_PTL_PID, NULL, NULL, &kibnal_ni);
        if (rc != PTL_OK && rc != PTL_IFACE_DUP) {
                ptl_unregister_nal(VIBNAL);
                return (-ENODEV);
        }
        
#ifdef CONFIG_SYSCTL
        /* Press on regardless even if registering sysctl doesn't work */
        kibnal_tunables.kib_sysctl = 
                register_sysctl_table (kibnal_top_ctl_table, 0);
#endif
        return (0);
}

MODULE_AUTHOR("Cluster File Systems, Inc. <info@clusterfs.com>");
MODULE_DESCRIPTION("Kernel Voltaire IB NAL v0.01");
MODULE_LICENSE("GPL");

module_init(kibnal_module_init);
module_exit(kibnal_module_fini);

