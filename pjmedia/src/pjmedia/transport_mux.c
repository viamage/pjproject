/* $Id$ */
/*
 * Copyright (C) 2008-2011 Teluu Inc. (http://www.teluu.com)
 * Copyright (C) 2018 Viamage LTD (http://viamage.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <pjmedia/transport_srtp.h>
#include <pjmedia/endpoint.h>
#include <pjlib-util/base64.h>
#include <pj/array.h>
#include <pj/assert.h>
#include <pj/ctype.h>
#include <pj/lock.h>
#include <pj/log.h>
#include <pj/os.h>
#include <pj/pool.h>

/* RTCP-mux transport */
typedef struct transport_mux
{
    pjmedia_transport	 base;		    /**< Base transport interface.  */
    pj_pool_t		*pool;		    /**< Pool for transport RTCP-mux.   */
    pj_lock_t		*mutex;		    /**< Mutex for libsrtp contexts.*/

    /* Stream information */
    void		*user_data;
    void		(*rtp_cb)( void *user_data,
				   void *pkt,
				   pj_ssize_t size);
    void		(*rtcp_cb)(void *user_data,
				   void *pkt,
				   pj_ssize_t size);

    /* Transport information */
    pjmedia_transport	*member_tp; /**< Underlying transport.       */
    pj_bool_t		 member_tp_attached;
    
} transport_mux;


/*
 * This callback is called by transport when incoming rtp is received
 */
static void mux_rtp_cb( void *user_data, void *pkt, pj_ssize_t size);

/*
 * This callback is called by transport when incoming rtcp is received
 */
static void mux_rtcp_cb( void *user_data, void *pkt, pj_ssize_t size);


static pj_status_t transport_get_info (pjmedia_transport *tp,
				       pjmedia_transport_info *info);
				       
static void	   transport_detach   (pjmedia_transport *tp,
				       void *strm);
static pj_status_t transport_send_rtp( pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size);
static pj_status_t transport_send_rtcp(pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size);
static pj_status_t transport_send_rtcp2(pjmedia_transport *tp,
				       const pj_sockaddr_t *addr,
				       unsigned addr_len,
				       const void *pkt,
				       pj_size_t size);
static pj_status_t transport_media_create(pjmedia_transport *tp,
				       pj_pool_t *sdp_pool,
				       unsigned options,
				       const pjmedia_sdp_session *sdp_remote,
				       unsigned media_index);
static pj_status_t transport_encode_sdp(pjmedia_transport *tp,
				       pj_pool_t *sdp_pool,
				       pjmedia_sdp_session *sdp_local,
				       const pjmedia_sdp_session *sdp_remote,
				       unsigned media_index);
static pj_status_t transport_media_start (pjmedia_transport *tp,
				       pj_pool_t *pool,
				       const pjmedia_sdp_session *sdp_local,
				       const pjmedia_sdp_session *sdp_remote,
				       unsigned media_index);
static pj_status_t transport_media_stop(pjmedia_transport *tp);
static pj_status_t transport_simulate_lost(pjmedia_transport *tp,
				       pjmedia_dir dir,
				       unsigned pct_lost);
static pj_status_t transport_destroy  (pjmedia_transport *tp);
static pj_status_t transport_attach2  (pjmedia_transport *tp,
				       pjmedia_transport_attach_param *param);

static pjmedia_transport_op transport_mux_op =
{
    &transport_get_info,
    NULL, //&transport_attach,
    &transport_detach,
    &transport_send_rtp,
    &transport_send_rtcp,
    &transport_send_rtcp2,
    &transport_media_create,
    &transport_encode_sdp,
    &transport_media_start,
    &transport_media_stop,
    &transport_simulate_lost,
    &transport_destroy,
    &transport_attach2
};

static const pj_str_t ID_RTCPMUX = { "rtcp-mux", 8 };

/*
 * Create an RTCP-MUX media transport.
 */
PJ_DEF(pj_status_t) pjmedia_transport_mux_create(
				       pjmedia_endpt *endpt,
				       pjmedia_transport *tp,
				       pjmedia_transport **p_tp)
{
    pj_pool_t *pool;
    transport_mux *mux;
    pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(endpt && tp && p_tp, PJ_EINVAL);

    pool = pjmedia_endpt_create_pool(endpt, "srtp%p", 1000, 1000);
    mux = PJ_POOL_ZALLOC_T(pool, transport_mux);

    mux->pool = pool;

    status = pj_lock_create_recursive_mutex(pool, pool->obj_name, &mux->mutex);
    if (status != PJ_SUCCESS) {
	pj_pool_release(pool);
	return status;
    }

    /* Initialize base pjmedia_transport */
    pj_memcpy(mux->base.name, pool->obj_name, PJ_MAX_OBJ_NAME);
    if (tp)
	mux->base.type = tp->type;
    else
	mux->base.type = PJMEDIA_TRANSPORT_TYPE_UDP;
    mux->base.op = &transport_mux_op;

    /* Set underlying transport */
    mux->member_tp = tp;

    /* Done */
    *p_tp = &mux->base;

    return PJ_SUCCESS;
}

static pj_status_t transport_attach2(pjmedia_transport *tp,
				     pjmedia_transport_attach_param *param)
{
    transport_mux *mux = (transport_mux*) tp;
    pjmedia_transport_attach_param member_param;
    pj_status_t status;

    PJ_ASSERT_RETURN(tp && param, PJ_EINVAL);

    /* Save the callbacks */
    pj_lock_acquire(mux->mutex);
    mux->rtp_cb = param->rtp_cb;
    mux->rtcp_cb = param->rtcp_cb;
    mux->user_data = param->user_data;
    pj_lock_release(mux->mutex);

    /* Attach self to member transport */
    member_param = *param;
    member_param.user_data = mux;
    member_param.rtp_cb = &mux_rtp_cb;
    member_param.rtcp_cb = &mux_rtcp_cb;
    status = pjmedia_transport_attach2(mux->member_tp, &member_param);
    if (status != PJ_SUCCESS) {
	pj_lock_acquire(mux->mutex);
	mux->rtp_cb = NULL;
	mux->rtcp_cb = NULL;
	mux->user_data = NULL;
	pj_lock_release(mux->mutex);
	return status;
    }

    mux->member_tp_attached = PJ_TRUE;
    return PJ_SUCCESS;
}

static void transport_detach(pjmedia_transport *tp, void *strm)
{
    transport_mux *mux = (transport_mux*) tp;

    PJ_UNUSED_ARG(strm);
    PJ_ASSERT_ON_FAIL(tp, return);

    if (mux->member_tp) {
	pjmedia_transport_detach(mux->member_tp, mux);
    }

    /* Clear up application infos from transport */
    pj_lock_acquire(mux->mutex);
    mux->rtp_cb = NULL;
    mux->rtcp_cb = NULL;
    mux->user_data = NULL;
    pj_lock_release(mux->mutex);
    mux->member_tp_attached = PJ_FALSE;
}


static pj_status_t transport_send_rtp( pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size)
{
    pj_status_t status;
    transport_mux *mux = (transport_mux*) tp;
    return pjmedia_transport_send_rtp(mux->member_tp, pkt, size);
}

static pj_status_t transport_send_rtcp(pjmedia_transport *tp,
				       const void *pkt,
				       pj_size_t size)
{
    return transport_send_rtcp2(tp, NULL, 0, pkt, size);
}

static pj_status_t transport_send_rtcp2(pjmedia_transport *tp,
				        const pj_sockaddr_t *addr,
				        unsigned addr_len,
				        const void *pkt,
				        pj_size_t size)
{
   pj_status_t status;
   transport_mux *mux = (transport_mux*) tp;
   return pjmedia_transport_send_rtp(mux->member_tp, pkt, size);
}

static pj_status_t transport_simulate_lost(pjmedia_transport *tp,
					   pjmedia_dir dir,
					   unsigned pct_lost)
{
    transport_mux *mux = (transport_mux *) tp;

    PJ_ASSERT_RETURN(tp, PJ_EINVAL);

    return pjmedia_transport_simulate_lost(mux->member_tp, dir, pct_lost);
}

static pj_status_t transport_destroy  (pjmedia_transport *tp)
{
    transport_mux *mux = (transport_mux *) tp;
    pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(tp, PJ_EINVAL);

    /* In case mutex is being acquired by other thread */
    pj_lock_acquire(mux->mutex);
    pj_lock_release(mux->mutex);

    pj_lock_destroy(mux->mutex);
    pj_pool_release(mux->pool);

    return PJ_SUCCESS;
}


/*
 * This callback is called by transport when incoming rtp is received
 */
static void mux_rtp_cb( void *user_data, void *pkt, pj_ssize_t size)
{
    transport_mux *mux = (transport_mux *) user_data;
    
    unsigned char* data = (unsigned char*) pkt;
    int type = (size >= 2) ? ((data[1]) & 0x7F) : 0;
    if(type >= 64 && type < 96) {
	mux->rtp_cb(mux->user_data, pkt, size);
    } else {
	mux->rtcp_cb(mux->user_data, pkt, size);
    }
    return;
}

/*
 * This callback is called by transport when incoming rtcp is received
 */
static void mux_rtcp_cb( void *user_data, void *pkt, pj_ssize_t size)
{
    transport_mux *mux = (transport_mux *) user_data;
    mux->rtcp_cb(mux->user_data, pkt, size);
    return;
}

static pj_status_t transport_media_create(pjmedia_transport *tp,
				          pj_pool_t *sdp_pool,
					  unsigned options,
				          const pjmedia_sdp_session *sdp_remote,
					  unsigned media_index)
{
    struct transport_mux *mux = (struct transport_mux*) tp;
    return pjmedia_transport_media_create(mux->member_tp, sdp_pool,
					    options, sdp_remote,
					    media_index);
}

static pj_status_t transport_encode_sdp(pjmedia_transport *tp,
					pj_pool_t *sdp_pool,
					pjmedia_sdp_session *sdp_local,
					const pjmedia_sdp_session *sdp_remote,
					unsigned media_index)
{
    pjmedia_sdp_media *m_loc;
    pjmedia_sdp_attr* a;
    
    struct transport_mux *mux = (struct transport_mux*) tp;
    pj_status_t last_err_st = PJ_EBUG;
    pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(tp && sdp_pool && sdp_local, PJ_EINVAL);
    
    m_loc = sdp_local->media[media_index];

    status = pjmedia_transport_encode_sdp(mux->member_tp, sdp_pool,
					  sdp_local, sdp_remote, media_index);
    
    a = pjmedia_sdp_attr_create(sdp_pool, ID_RTCPMUX.ptr, NULL);
    pjmedia_sdp_media_add_attr(m_loc, a);
	
    return status;
}


static pj_status_t transport_media_start(pjmedia_transport *tp,
				         pj_pool_t *pool,
				         const pjmedia_sdp_session *sdp_local,
				         const pjmedia_sdp_session *sdp_remote,
				         unsigned media_index)
{
    struct transport_mux *mux = (struct transport_mux*) tp;
    pj_status_t last_err_st = PJ_EBUG;
    pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(tp && pool && sdp_local && sdp_remote, PJ_EINVAL);

    status = pjmedia_transport_media_start(mux->member_tp, pool,
					   sdp_local, sdp_remote,
				           media_index);    
    return status;
}

static pj_status_t transport_media_stop(pjmedia_transport *tp)
{
    struct transport_mux *mux = (struct transport_mux*) tp;
    pj_status_t status;
    unsigned i;

    PJ_ASSERT_RETURN(tp, PJ_EINVAL);

    /* Invoke media_stop() of member tp */
    status = pjmedia_transport_media_stop(mux->member_tp);
    if (status != PJ_SUCCESS)
	PJ_LOG(4, (mux->pool->obj_name,
		   "RTCP-mux failed stop underlying media transport."));
    return status;
}


static pj_status_t transport_get_info(pjmedia_transport *tp,
				      pjmedia_transport_info *info)
{
    transport_mux *mux = (transport_mux*) tp;
    
    return pjmedia_transport_get_info(mux->member_tp, info);
}

