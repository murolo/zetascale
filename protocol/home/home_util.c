//----------------------------------------------------------------------------
// ZetaScale
// Copyright (c) 2016, SanDisk Corp. and/or all its affiliates.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License version 2.1 as published by the Free
// Software Foundation;
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License v2.1 for more details.
//
// A copy of the GNU Lesser General Public License v2.1 is provided with this package and
// can also be found at: http://opensource.org/licenses/LGPL-2.1
// You should have received a copy of the GNU Lesser General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 59 Temple
// Place, Suite 330, Boston, MA 02111-1307 USA.
//----------------------------------------------------------------------------

/*
 * File:   sdf/protocol/home/util.c
 *
 * Author: Brian O'Krafka
 *
 * Created on March 30, 2008
 *
 * (c) Copyright 2008, Schooner Information Technology, Inc.
 * http://www.schoonerinfotech.com/
 *
 * $Id: home_util.c 8808 2009-08-19 17:59:56Z jmoilanen $
 */

#include "home_util.h"

struct sdf_msg *
home_load_msg(SDF_vnode_t node_from, SDF_vnode_t node_to,
              SDF_protocol_msg_t *pm_old, SDF_protocol_msg_type_t msg_type,
              void *data, SDF_size_t data_size, 
              SDF_time_t exptime, SDF_time_t createtime, uint64_t sequence,
              SDF_status_t status, SDF_size_t *pmsize, char * key, int key_len,
              uint32_t flags)
{
    uint64_t             fmt;
    SDF_size_t           msize;
    sdf_msg_t           *msg = NULL;
    SDF_protocol_msg_t  *pm_new = NULL;
    unsigned char       *pdata_msg;

    fmt = SDF_Protocol_Msg_Info[msg_type].format;

    plat_assert_imply(fmt & m_data, data || !data_size);
    plat_assert_imply(data, fmt & m_data);
    plat_assert_iff(fmt & m_key_, key);

    msize = sizeof(SDF_protocol_msg_t);
    if ((data_size > 0) && (fmt & m_data)) {
        msize += data_size;
	msg    = sdf_msg_alloc(msize);
	#ifdef MALLOC_TRACE
	    UTMallocTrace("home_load_msg", FALSE, FALSE, FALSE, (void *) msg, msize);
	#endif // MALLOC_TRACE
	if (msg == NULL) {
	    UTError("sdf_msg_alloc returned NULL");
	}
	pm_new = (SDF_protocol_msg_t *) msg->msg_payload;
	pdata_msg = (unsigned char *) pm_new + sizeof(SDF_protocol_msg_t);
	(void) memcpy((void *) pdata_msg, data, data_size);
    } else {
	msg    = (struct sdf_msg *) sdf_msg_alloc(msize);
	#ifdef MALLOC_TRACE
	    UTMallocTrace("home_load_msg", FALSE, FALSE, FALSE, (void *) msg, msize);
	#endif // MALLOC_TRACE
	if (msg == NULL) {
	    UTError("sdf_msg_alloc returned NULL");
	}
	pm_new = (SDF_protocol_msg_t *) msg->msg_payload;
    }

    /* XXX: drew 2009-05-31 Handle overflow keys if we ever do that */
    plat_assert(key_len <= sizeof (pm_new->key.key));
    if (key) {
        memcpy(&pm_new->key.key, key, key_len);
        pm_new->key.len = key_len;
    }

    pm_new->current_version = PROTOCOL_MSG_VERSION;
    pm_new->supported_version = PROTOCOL_MSG_VERSION;
    pm_new->data_offset = 0;
    pm_new->data_size   = data_size;
    pm_new->flags       = flags;
    pm_new->msgtype     = msg_type;
    pm_new->shard       = pm_old->shard;
    pm_new->tag         = pm_old->tag;
    pm_new->thrd        = pm_old->thrd;
    pm_new->node_from   = node_from;
    pm_new->node_to     = node_to;
    pm_new->status      = status;
    pm_new->exptime     = exptime;
    pm_new->createtime  = createtime;
    pm_new->seqno	= sequence;

    if (pmsize) {
        *pmsize = msize;
    }
    return(msg);
}
