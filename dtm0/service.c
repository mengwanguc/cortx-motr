/* -*- C -*- */
/*
 * Copyright (c) 2021 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */

#define M0_TRACE_SUBSYSTEM M0_TRACE_SUBSYS_DTM0
#include "lib/trace.h"
#include "dtm0/addb2.h"
#include "dtm0/service.h"
#include "lib/string.h"              /* streq */
#include "lib/errno.h"               /* ENOMEM and so on */
#include "lib/memory.h"              /* M0_ALLOC_PTR */
#include "reqh/reqh_service.h"       /* m0_reqh_service */
#include "reqh/reqh.h"               /* m0_reqh */
#include "rpc/rpc_machine.h"         /* m0_rpc_machine */
#include "dtm0/fop.h"                /* dtm0_fop */
#include "dtm0/dtx.h"                /* dtx_domain_init */
#include "lib/tlist.h"               /* tlist API */
#include "be/dtm0_log.h"             /* DTM0 log API */
#include "module/instance.h"         /* m0_get */
#include "lib/coroutine.h"           /* m0_co API */
#include "rpc/rpc_opcodes.h"         /* M0_DTM0_{RLINK,REQ}_OPCODE */
#include "rpc/rpc.h"                 /* m0_rpc_item_post */

#include "conf/confc.h"   /* m0_confc */
#include "conf/diter.h"   /* m0_conf_diter */
#include "conf/obj_ops.h" /* M0_CONF_DIRNEXT */
#include "conf/helpers.h" /* m0_confc_root_open, m0_conf_process2service_get */
#include "reqh/reqh.h"    /* m0_reqh2confc */

static int dtm0_service__ha_subscribe(struct m0_reqh_service *service);
static void dtm0_service__ha_unsubscribe(struct m0_reqh_service *service);

static struct m0_dtm0_service *to_dtm(struct m0_reqh_service *service);
static int dtm0_service_start(struct m0_reqh_service *service);
static void dtm0_service_stop(struct m0_reqh_service *service);
static void dtm0_service_prepare_to_stop(struct m0_reqh_service *service);
static int dtm0_service_allocate(struct m0_reqh_service **service,
				 const struct m0_reqh_service_type *stype);
static void dtm0_service_fini(struct m0_reqh_service *service);
static int  m0_dtm0_rpc_link_mod_init(void);
static void m0_dtm0_rpc_link_mod_fini(void);

/* Settings for RPC connections with DTM0 services. */
enum {
	DTM0_MAX_RPCS_IN_FLIGHT = 10,
	DTM0_DISCONNECT_TIMEOUT_SECS = 5
};

static const struct m0_reqh_service_type_ops dtm0_service_type_ops = {
	.rsto_service_allocate = dtm0_service_allocate
};

static const struct m0_reqh_service_ops dtm0_service_ops = {
	.rso_start           = dtm0_service_start,
	.rso_stop            = dtm0_service_stop,
	.rso_fini            = dtm0_service_fini,
	.rso_prepare_to_stop = dtm0_service_prepare_to_stop,
};

struct m0_reqh_service_type dtm0_service_type = {
	.rst_name  = "M0_CST_DTM0",
	.rst_ops   = &dtm0_service_type_ops,
	.rst_level = M0_RS_LEVEL_LATE,
};


/**
 * System process which dtm0 subscribes for state updates
 */
struct dtm0_process {
	struct m0_tlink         dop_link;
	uint64_t                dop_magic;

	/**
	 * Listens for an event on process conf object's HA channel.
	 * Updates dtm0_process status in the clink callback on HA notification.
	 */
	struct m0_clink         dop_ha_link;
	/**
	 * Link connected to remote process
	 */
	struct m0_rpc_link      dop_rlink;
	/**
	 * Remote process fid
	 */
	struct m0_fid           dop_rproc_fid;
	/**
	 * Remote service fid
	 */
	struct m0_fid           dop_rserv_fid;
	/**
	 * Remote process endpoint
	 */
	const char             *dop_rep;
	/**
	 * Current dtm0 service dtm0 process to.
	 */
	struct m0_reqh_service *dop_dtm0_service;
	/** Protects ::dop_rlink from concurrent access from different FOMs. */
	struct m0_long_lock     dop_llock;
	/**
	 * Connect ast
	 */
	struct m0_sm_ast        dop_service_connect_ast;
	struct m0_clink         dop_service_connect_clink;
};

M0_TL_DESCR_DEFINE(dopr, "dtm0_process", static, struct dtm0_process, dop_link,
		   dop_magic, M0_DTM0_PROC_MAGIC, M0_DTM0_PROC_HEAD_MAGIC);
M0_TL_DEFINE(dopr, static, struct dtm0_process);

/**
 * typed container_of
 */
static const struct m0_bob_type dtm0_service_bob = {
	.bt_name = "dtm0 service",
	.bt_magix_offset = M0_MAGIX_OFFSET(struct m0_dtm0_service, dos_magix),
	.bt_magix = M0_DTM0_SVC_MAGIC,
	.bt_check = NULL
};
M0_BOB_DEFINE(static, &dtm0_service_bob, m0_dtm0_service);

static struct m0_dtm0_service *to_dtm(struct m0_reqh_service *service)
{
	return bob_of(service, struct m0_dtm0_service, dos_generic,
		      &dtm0_service_bob);
}

/**
 * Service part
 */
static void dtm0_service__init(struct m0_dtm0_service *s)
{
	m0_dtm0_service_bob_init(s);
	dopr_tlist_init(&s->dos_processes);
	m0_dtm0_dtx_domain_init();
	m0_dtm0_clk_src_init(&s->dos_clk_src, M0_DTM0_CS_PHYS);
}

static void dtm0_service__fini(struct m0_dtm0_service *s)
{
	m0_dtm0_clk_src_fini(&s->dos_clk_src);
	m0_dtm0_dtx_domain_fini();
	dopr_tlist_fini(&s->dos_processes);
	m0_dtm0_service_bob_fini(s);
}

M0_INTERNAL int
m0_dtm_client_service_start(struct m0_reqh *reqh, struct m0_fid *cli_srv_fid,
			    struct m0_reqh_service **out)
{
	struct m0_reqh_service_type *svct;
	struct m0_reqh_service      *reqh_svc;
	int                          rc;

	svct = m0_reqh_service_type_find("M0_CST_DTM0");
	if (svct == NULL)
		return M0_ERR(-ENOENT);

	rc = m0_reqh_service_allocate(&reqh_svc, svct, NULL);
	if (rc != 0)
		return M0_ERR(rc);

	m0_reqh_service_init(reqh_svc, reqh, cli_srv_fid);

	rc = m0_reqh_service_start(reqh_svc);
	if (rc != 0)
		m0_reqh_service_fini(reqh_svc);
	else
		*out = reqh_svc;

	return M0_RC(rc);
}

M0_INTERNAL void m0_dtm_client_service_stop(struct m0_reqh_service *svc)
{
       m0_reqh_service_prepare_to_stop(svc);
       m0_reqh_idle_wait_for(svc->rs_reqh, svc);
       m0_reqh_service_stop(svc);
       m0_reqh_service_fini(svc);
}


static struct dtm0_process *
dtm0_service_process__lookup(struct m0_reqh_service *reqh_dtm0_svc,
			     const struct m0_fid    *remote_dtm0)
{
	return m0_tl_find(dopr, proc, &to_dtm(reqh_dtm0_svc)->dos_processes,
			  m0_fid_eq(&proc->dop_rserv_fid, remote_dtm0));
}


M0_INTERNAL int
m0_dtm0_service_process_connect(struct m0_reqh_service *s,
				struct m0_fid          *remote_srv,
				const char             *remote_ep,
				bool                    async)
{
	struct dtm0_process   *process;
	struct m0_rpc_machine *mach =
		m0_reqh_rpc_mach_tlist_head(&s->rs_reqh->rh_rpc_machines);
	int                    rc;

	process = dtm0_service_process__lookup(s, remote_srv);
	if (process == NULL)
		return M0_RC(-ENOENT);

	rc = m0_rpc_link_init(&process->dop_rlink, mach, remote_srv,
			      remote_ep, DTM0_MAX_RPCS_IN_FLIGHT);
	if (rc != 0)
		return M0_ERR(rc);

	M0_LOG(M0_DEBUG, "async=%d, dtm0="FID_F", remote_srv="FID_F", rep=%s",
	       !!async, FID_P(&s->rs_service_fid), FID_P(remote_srv),
	       remote_ep);

	if (async)
		m0_rpc_link_connect_async(&process->dop_rlink,
					  M0_TIME_NEVER,
					  &process->dop_service_connect_clink);
	else
		rc = m0_rpc_link_connect_sync(&process->dop_rlink,
					      M0_TIME_NEVER);

	return M0_RC(rc);
}

static int dtm0_process_disconnect(struct dtm0_process *process)
{
	int                  rc;
	const m0_time_t      timeout =
		m0_time_from_now(DTM0_DISCONNECT_TIMEOUT_SECS, 0);

	M0_ENTRY("process=%p, rfid=" FID_F, process,
		 FID_P(&process->dop_rserv_fid));

	if (M0_IS0(&process->dop_rlink))
		return M0_RC(0);

	rc = m0_rpc_link_is_connected(&process->dop_rlink) ?
		m0_rpc_link_disconnect_sync(&process->dop_rlink, timeout) : 0;

	if (M0_IN(rc, (0, -ETIMEDOUT, -ECANCELED))) {
		/* TODO: Fix this. We are ignoring -ECANCELED for now.*/
		if (rc == -ETIMEDOUT || rc == -ECANCELED) {
			M0_LOG(M0_WARN, "Disconnect %s (suppressed)",
			       rc == -ETIMEDOUT ? "timed out" : "cancelled");
			rc = 0;
		}

		m0_rpc_link_fini(&process->dop_rlink);
		M0_SET0(&process->dop_rlink);
	}

	return M0_RC(rc);
}

M0_INTERNAL int
m0_dtm0_service_process_disconnect(struct m0_reqh_service *s,
				   struct m0_fid          *remote_srv)
{
	struct dtm0_process *process =
		dtm0_service_process__lookup(s, remote_srv);

	M0_ENTRY("rs=%p, remote="FID_F, s, FID_P(remote_srv));

	return process == NULL ? M0_ERR(-ENOENT) :
		M0_RC(dtm0_process_disconnect(process));
}

M0_INTERNAL struct m0_rpc_session *
m0_dtm0_service_process_session_get(struct m0_reqh_service *s,
				    const struct m0_fid    *remote_srv)
{
	struct dtm0_process *process =
		dtm0_service_process__lookup(s, remote_srv);

	return process == NULL ? NULL : &process->dop_rlink.rlk_sess;
}

static int dtm0_service__alloc(struct m0_reqh_service           **service,
			       const struct m0_reqh_service_type *stype,
			       const struct m0_reqh_service_ops  *ops)
{
	struct m0_dtm0_service *s;

	M0_PRE(stype != NULL && service != NULL && ops != NULL);

	M0_ALLOC_PTR(s);
	if (s == NULL)
		return M0_ERR(-ENOMEM);

	s->dos_generic.rs_type = stype;
	s->dos_generic.rs_ops  = ops;
	dtm0_service__init(s);
	*service = &s->dos_generic;

	return M0_RC(0);
}

static int dtm0_service_allocate(struct m0_reqh_service           **service,
				 const struct m0_reqh_service_type *stype)
{
	return dtm0_service__alloc(service, stype, &dtm0_service_ops);
}

static int dtm_service__origin_fill(struct m0_reqh_service *service)
{
	struct m0_conf_service *service_obj;
	struct m0_conf_obj     *obj;
	struct m0_confc        *confc = m0_reqh2confc(service->rs_reqh);
	const char            **param;
	struct m0_dtm0_service *dtm0 = to_dtm(service);
	int                     rc;

	M0_ENTRY("rs_svc=%p", service);

	/* W/A for UTs */
	if (!m0_confc_is_inited(confc)) {
		dtm0->dos_origin = DTM0_ON_VOLATILE;
		goto out;
	}

	obj = m0_conf_cache_lookup(&confc->cc_cache, &service->rs_service_fid);
	if (obj == NULL)
		return M0_ERR(-ENOENT);

	service_obj = M0_CONF_CAST(obj, m0_conf_service);

	if (service_obj->cs_params == NULL) {
		dtm0->dos_origin = DTM0_ON_VOLATILE;
		M0_LOG(M0_WARN, "dtm0 is treated as volatile,"
		       " no parameters given");
		goto out;
	}

	for (param = service_obj->cs_params; *param != NULL; ++param) {
		if (m0_streq(*param, "origin:in-volatile"))
			dtm0->dos_origin = DTM0_ON_VOLATILE;
		else if (m0_streq(*param, "origin:in-persistent"))
			dtm0->dos_origin = DTM0_ON_PERSISTENT;
	}

	if (dtm0->dos_origin == DTM0_ON_PERSISTENT) {
		dtm0->dos_log = m0_reqh_lockers_get(service->rs_reqh,
						    m0_get()->i_dtm0_log_key);
		M0_ASSERT(dtm0->dos_log != NULL);
	}

out:
	rc = 0;
	if (dtm0->dos_origin == DTM0_ON_VOLATILE)
		rc = m0_be_dtm0_log_alloc(&dtm0->dos_log);

	if (rc == 0) {
		rc = m0_be_dtm0_log_init(
			dtm0->dos_log,
			service->rs_reqh->rh_beseg,
			&dtm0->dos_clk_src,
			dtm0->dos_origin == DTM0_ON_PERSISTENT);
		if (rc != 0)
			m0_be_dtm0_log_free(&dtm0->dos_log);
	}

	return M0_RC_INFO(rc, "origin=%d", dtm0->dos_origin);
}

static int dtm0_service_start(struct m0_reqh_service *service)
{
        M0_PRE(service != NULL);
        return dtm_service__origin_fill(service) ?:
		dtm0_service__ha_subscribe(service);
}

static void dtm0_service_prepare_to_stop(struct m0_reqh_service *service)
{
	dtm0_service__ha_unsubscribe(service);
}

static void dtm0_service_stop(struct m0_reqh_service *service)
{
	struct m0_dtm0_service *dtm0;

	M0_PRE(service != NULL);
	dtm0 = to_dtm(service);

	m0_dtm0_fop_fini();
	/**
	 * It is safe to remove any remaining entries from the log
	 * when a process with volatile log is going to die.
	 */
	if (dtm0->dos_origin == DTM0_ON_VOLATILE && dtm0->dos_log != NULL) {
		m0_be_dtm0_log_clear(dtm0->dos_log);
		m0_be_dtm0_log_fini(dtm0->dos_log);
		m0_be_dtm0_log_free(&dtm0->dos_log);
	}
}

static void dtm0_service_fini(struct m0_reqh_service *service)
{
	M0_PRE(service != NULL);
	dtm0_service__fini(to_dtm(service));
	m0_free(service);
}

M0_INTERNAL int m0_dtm0_stype_init(void)
{
	extern struct m0_sm_conf m0_dtx_sm_conf;

	return m0_sm_addb2_init(&m0_dtx_sm_conf,
				M0_AVI_DTX0_SM_STATE, M0_AVI_DTX0_SM_COUNTER) ?:
		m0_dtm0_fop_init() ?:
		m0_reqh_service_type_register(&dtm0_service_type) ?:
		m0_dtm0_rpc_link_mod_init();
}

M0_INTERNAL void m0_dtm0_stype_fini(void)
{
	extern struct m0_sm_conf m0_dtx_sm_conf;
	m0_dtm0_rpc_link_mod_fini();
	m0_reqh_service_type_unregister(&dtm0_service_type);
	m0_sm_addb2_fini(&m0_dtx_sm_conf);
}

M0_INTERNAL bool m0_dtm0_is_a_volatile_dtm(struct m0_reqh_service *service)
{
	return m0_streq(service->rs_type->rst_name, "M0_CST_DTM0") &&
		to_dtm(service)->dos_origin == DTM0_ON_VOLATILE;
}

M0_INTERNAL bool m0_dtm0_is_a_persistent_dtm(struct m0_reqh_service *service)
{
	return m0_streq(service->rs_type->rst_name, "M0_CST_DTM0") &&
		to_dtm(service)->dos_origin == DTM0_ON_PERSISTENT;
}

M0_INTERNAL struct m0_dtm0_service *
m0_dtm0_service_find(const struct m0_reqh *reqh)
{
	struct m0_reqh_service *rh_srv;

	rh_srv = m0_reqh_service_find(&dtm0_service_type, reqh);

	return rh_srv == NULL ? NULL : to_dtm(rh_srv);
}

M0_INTERNAL bool m0_dtm0_in_ut(void)
{
	return M0_FI_ENABLED("ut");
}

/* --------------------------------- EVENTS --------------------------------- */

static int ready_to_process_ha_msgs = 0;

static bool service_connect_clink(struct m0_clink *link)
{
	M0_ENTRY();
	M0_LOG(M0_DEBUG, "DTM0 service: connected");
	M0_LEAVE();
	return true;
}

static void service_connect_ast(struct m0_sm_group *grp,
				struct m0_sm_ast   *ast)
{
	struct dtm0_process *process = ast->sa_datum;
	int rc;

	M0_ENTRY();
	m0_clink_init(&process->dop_service_connect_clink, service_connect_clink);
	process->dop_service_connect_clink.cl_is_oneshot = true;
	rc = m0_dtm0_service_process_connect(process->dop_dtm0_service,
					     &process->dop_rserv_fid,
					     process->dop_rep,
					     true);
	M0_ASSERT(rc == 0);
	M0_LEAVE();
}

static bool process_clink_cb(struct m0_clink *clink)
{
	struct dtm0_process    *process    = M0_AMB(process, clink,
						    dop_ha_link);
	struct m0_conf_obj     *process_obj = container_of(clink->cl_chan,
						       struct m0_conf_obj,
						       co_ha_chan);
	struct m0_reqh_service *dtm0 = process->dop_dtm0_service;
	struct m0_reqh         *reqh = dtm0->rs_reqh;
	struct m0_fid          *current_proc_fid = &reqh->rh_fid;
	struct m0_fid          *evented_proc_fid = &process_obj->co_id;
	enum m0_ha_obj_state    evented_proc_state = process_obj->co_ha_state;

	M0_ENTRY();
	M0_PRE(m0_conf_obj_type(process_obj) == &M0_CONF_PROCESS_TYPE);
	M0_PRE(m0_fid_eq(evented_proc_fid, &process->dop_rproc_fid));
	M0_PRE(!m0_fid_eq(evented_proc_fid, &M0_FID0));
	M0_PRE(!m0_fid_eq(evented_proc_fid, current_proc_fid));

	if (m0_dtm0_in_ut()) {
		M0_LOG(M0_DEBUG, "No HA handling in UT.");
		goto out;
	}

	/**
	 * A weird logic to make a workaround for current HA
	 * implementation: skip processing of all HA messages
	 * until TRANSIENT is received. TRANSIENT is just a
	 * trigger for handling HA messages in this case.
	 */
	if (evented_proc_state != M0_NC_TRANSIENT &&
	    !ready_to_process_ha_msgs) {
		M0_LOG(M0_DEBUG, "Is not ready to process HA messages");
		goto out;
	}

	M0_LOG(M0_DEBUG,
	       " evented_proc_state=%d"
	       " evented_proc_fid="FID_F
	       " dop_rserv_fid="FID_F
	       " dop_rproc_fid="FID_F
	       " curr_proc_fid="FID_F,
	       evented_proc_state,
	       FID_P(evented_proc_fid),
	       FID_P(&process->dop_rserv_fid),
	       FID_P(&process->dop_rproc_fid),
	       FID_P(current_proc_fid));
	M0_LOG(M0_DEBUG, "dtm0=%p dop_rep=%s", dtm0, process->dop_rep);

	switch (evented_proc_state) {
	case M0_NC_ONLINE:
		process->dop_service_connect_ast = (struct m0_sm_ast){
			.sa_cb    = &service_connect_ast,
			.sa_datum = process,
		};
		m0_sm_ast_post(m0_locality_here()->lo_grp,
			       &process->dop_service_connect_ast);
		break;
	case M0_NC_TRANSIENT:
		ready_to_process_ha_msgs = 1;
		M0_LOG(M0_DEBUG, "TRANSIENT received, now is "
		       "ready to process HA messages");
		break;
	default:
		M0_LOG(M0_DEBUG, "Ignored received proc state %d",
		       evented_proc_state);
		break;
	}
out:
	M0_LEAVE();

	return false;
}

static void dtm0_process__ha_state_subscribe(struct dtm0_process *process,
					     struct m0_conf_obj  *obj)
{
	M0_ENTRY();
	m0_clink_init(&process->dop_ha_link, process_clink_cb);
	m0_clink_add_lock(&obj->co_ha_chan, &process->dop_ha_link);
	M0_LEAVE();
}

static void dtm0_process__ha_state_unsubscribe(struct dtm0_process *process)
{
	M0_ENTRY();
	m0_clink_del_lock(&process->dop_ha_link);
	m0_clink_fini(&process->dop_ha_link);
	M0_LEAVE();
}

static bool conf_obj_is_process(const struct m0_conf_obj *obj)
{
	return m0_conf_obj_type(obj) == &M0_CONF_PROCESS_TYPE;
}

static int dtm0_service__ha_subscribe(struct m0_reqh_service *service)
{
	struct m0_confc        *confc;
	struct m0_conf_root    *root;
	struct m0_conf_diter    it;
	struct m0_conf_obj     *obj;
	struct m0_conf_process *process;
	struct dtm0_process    *dtm0_process;
	struct m0_dtm0_service *s;
	struct m0_fid           rproc_fid;
	struct m0_fid           rserv_fid;
	struct m0_fid          *current_proc_fid;
	int                     rc;

	M0_ENTRY();

	M0_PRE(service != NULL);

	confc = m0_reqh2confc(service->rs_reqh);
	current_proc_fid = &service->rs_reqh->rh_fid;
	s = container_of(service, struct m0_dtm0_service, dos_generic);

	/** UT workaround */
	if (!m0_confc_is_inited(confc)) {
		M0_LOG(M0_WARN, "confc is not initiated!");
		return M0_RC(0);
	}

	rc = m0_confc_root_open(confc, &root);
	if (rc != 0)
		return M0_ERR(rc);

	rc = m0_conf_diter_init(&it, confc,
				&root->rt_obj,
				M0_CONF_ROOT_NODES_FID,
				M0_CONF_NODE_PROCESSES_FID);
	if (rc != 0) {
		m0_confc_close(&root->rt_obj);
		return M0_ERR(rc);
	}

	while ((rc = m0_conf_diter_next_sync(&it, conf_obj_is_process)) > 0) {
		obj = m0_conf_diter_result(&it);
		process = M0_CONF_CAST(obj, m0_conf_process);
		rproc_fid = process->pc_obj.co_id;
		rc = m0_conf_process2service_get(confc, &rproc_fid,
						 M0_CST_DTM0, &rserv_fid);
		if (rc == 0) {
			if (m0_fid_eq(&rproc_fid, current_proc_fid))
				/* skip current process */
				continue;
			M0_ALLOC_PTR(dtm0_process);
			M0_ASSERT(dtm0_process != NULL); /* XXX */
			dtm0_process__ha_state_subscribe(dtm0_process, obj);
			dopr_tlink_init(dtm0_process);
			dopr_tlist_add(&s->dos_processes, dtm0_process);
			dtm0_process->dop_rproc_fid = rproc_fid;
			dtm0_process->dop_rserv_fid = rserv_fid;
			dtm0_process->dop_rep =
				m0_strdup(process->pc_endpoint);
			dtm0_process->dop_dtm0_service = service;
		}
	}

	m0_conf_diter_fini(&it);
	m0_confc_close(&root->rt_obj);

	return M0_RC(0);
}

static void dtm0_service__ha_unsubscribe(struct m0_reqh_service *reqh_service)
{
	struct dtm0_process    *process;
	struct m0_dtm0_service *service;
	int                     rc;

	M0_PRE(reqh_service != NULL);
	service = container_of(reqh_service, struct m0_dtm0_service,
			       dos_generic);

	M0_ENTRY("dtms=%p", service);

	while ((process = dopr_tlist_pop(&service->dos_processes)) != NULL) {
		rc = dtm0_process_disconnect(process);
		M0_ASSERT_INFO(rc == 0, "TODO: Disconnect failures"
			       " are not handled yet.");
		dtm0_process__ha_state_unsubscribe(process);
		dopr_tlink_fini(process);
		m0_free(process);
	}

	M0_LEAVE();
}

#if !defined(__KERNEL__)
/* DTM0 RPC Link is a lazy ("on-demand") RPC link. */
struct drlink_fom {
	struct m0_fom            df_gen;
	struct m0_co_context     df_co;
	struct m0_fid            df_tgt;
	struct m0_fop           *df_rfop;
	struct m0_dtm0_service  *df_svc;
};

static struct drlink_fom *fom2drlink_fom(struct m0_fom *fom)
{
	struct drlink_fom *df;
	df = M0_AMB(df, fom, df_gen);
	return df;
}

static size_t drlink_fom_locality(const struct m0_fom *fom)
{
	static size_t loc;
	return loc++;
}

static void drlink_fom_fini(struct m0_fom *fom)
{
	struct drlink_fom *df = fom2drlink_fom(fom);
	m0_fop_put_lock(df->df_rfop);
}

static int  drlink_fom_tick(struct m0_fom *fom);

static const struct m0_fom_ops drlink_fom_ops = {
	.fo_fini          = drlink_fom_fini,
	.fo_tick          = drlink_fom_tick,
	.fo_home_locality = drlink_fom_locality
};

static struct m0_fom_type drlink_fom_type;

static const struct m0_fom_type_ops drlink_fom_type_ops = {
	.fto_create = NULL
};

const static struct m0_sm_conf drlink_fom_conf;

static int m0_dtm0_rpc_link_mod_init(void)
{
	m0_fom_type_init(&drlink_fom_type,
			 M0_DTM0_RLINK_OPCODE,
			 &drlink_fom_type_ops,
			 &dtm0_service_type,
			 &drlink_fom_conf);

	return 0;
}

static void m0_dtm0_rpc_link_mod_fini(void)
{
}

/* creates a deep copy of the given request */
static struct dtm0_req_fop *dtm0_req_fop_dup(const struct dtm0_req_fop *src)
{
	int                  rc;
	struct dtm0_req_fop *dst;

	M0_ALLOC_PTR(dst);
	if (dst == NULL)
		return NULL;

	rc = m0_dtm0_tx_desc_copy(&src->dtr_txr, &dst->dtr_txr);
	if (rc != 0) {
		M0_ASSERT(rc == -ENOMEM);
		m0_free(dst);
		return NULL;
	}

	dst->dtr_msg = src->dtr_msg;

	return dst;
}

static void dtm0_req_fop_fini(struct dtm0_req_fop *req)
{
	m0_dtm0_tx_desc_fini(&req->dtr_txr);
}

static int drlink_fom_init(struct drlink_fom            *fom,
			   struct m0_dtm0_service       *svc,
			   const struct m0_fid          *tgt,
			   const struct dtm0_req_fop    *req)
{
	struct m0_rpc_machine  *mach;
	struct m0_reqh         *reqh;
	struct dtm0_req_fop    *owned_req;
	struct m0_fop          *fop;

	M0_ENTRY();
	M0_PRE(fom != NULL);
	M0_PRE(svc != NULL);
	M0_PRE(req != NULL);
	M0_PRE(m0_fid_is_valid(tgt));

	reqh = svc->dos_generic.rs_reqh;
	mach = m0_reqh_rpc_mach_tlist_head(&reqh->rh_rpc_machines);

	owned_req = dtm0_req_fop_dup(req);
	if (owned_req == NULL)
		return M0_ERR(-ENOMEM);

	fop = m0_fop_alloc(&dtm0_req_fop_fopt, owned_req, mach);
	if (fop == NULL) {
		dtm0_req_fop_fini(owned_req);
		m0_free(owned_req);
		return M0_ERR(-ENOMEM);
	}

	fop->f_opaque = fom;

	m0_fom_init(&fom->df_gen, &drlink_fom_type, &drlink_fom_ops,
		    NULL, NULL, reqh);

	/* TODO: can we use fom->fo_fop instead? */
	fom->df_rfop =  fop;
	fom->df_svc  =  svc;
	fom->df_tgt  = *tgt;

	m0_co_context_init(&fom->df_co);

	M0_LEAVE();

	return M0_RC(0);
}


static void co_long_write_lock(struct m0_co_context *context,
			       struct m0_long_lock *lk,
			       struct m0_long_lock_link *link,
			       int next_phase)
{
	M0_CO_REENTER(context);
	if (!m0_long_write_lock(lk, link, next_phase))
		M0_CO_YIELD(context, -EINTR);
}

enum {
	/* One second of waiting until we get -ETIMEOUT. */
	M0_DTM0_RLINK_CONNECT_TIMEOUT = 1,
};

static void co_rpc_link_connect(struct m0_co_context *context,
				struct m0_rpc_link *rlink,
				struct m0_fom *fom,
				int next_phase)
{
	const m0_time_t deadline =
		m0_time_from_now(M0_DTM0_RLINK_CONNECT_TIMEOUT, 0);
	M0_CO_REENTER(context);

	m0_chan_lock(&rlink->rlk_wait);
	m0_fom_wait_on(fom, &rlink->rlk_wait, &fom->fo_cb);
	m0_chan_unlock(&rlink->rlk_wait);

	m0_rpc_link_connect_async(rlink, deadline, NULL);
	m0_fom_phase_set(fom, next_phase);

	M0_CO_YIELD(context, -EINTR);
}

static int dtm0_process_init(struct dtm0_process    *proc,
			     struct m0_dtm0_service *dtms,
			     const struct m0_fid    *rem_svc_fid)
{
	struct m0_conf_process *rem_proc_conf;
	struct m0_conf_service *rem_svc_conf;
	struct m0_conf_obj     *obj;
	struct m0_reqh         *reqh;
	struct m0_conf_cache   *cache;

	/* TODO: M0_PRE dtms is locked */
	M0_ENTRY();

	reqh = dtms->dos_generic.rs_reqh;
	cache = &m0_reqh2confc(reqh)->cc_cache;

	obj = m0_conf_cache_lookup(cache, rem_svc_fid);
	if (obj == NULL)
		return M0_ERR_INFO(-ENOENT,
				   "Cannot find svc in the conf cache.");
	rem_svc_conf = M0_CONF_CAST(obj, m0_conf_service);
	obj = m0_conf_obj_grandparent(obj);
	if (obj == NULL)
		return M0_ERR_INFO(-ENOENT,
				   "Cannot find proc in the conf cache.");
	rem_proc_conf = M0_CONF_CAST(obj, m0_conf_process);

	if (rem_svc_conf->cs_type != M0_CST_DTM0)
		return M0_ERR_INFO(-ENOENT, "Not a DTM0 service.");

	dopr_tlink_init(proc);
	dopr_tlist_add(&dtms->dos_processes, proc);

	proc->dop_rproc_fid = rem_proc_conf->pc_obj.co_id;
	proc->dop_rserv_fid = rem_svc_conf->cs_obj.co_id;

	proc->dop_rep = m0_strdup(rem_proc_conf->pc_endpoint);
	proc->dop_dtm0_service = &dtms->dos_generic;

	m0_long_lock_init(&proc->dop_llock);

	return M0_RC(0);
}

static int find_or_add(struct m0_dtm0_service *dtms,
		       const struct m0_fid    *tgt,
		       struct dtm0_process   **out)
{
	struct dtm0_process *process;
	int                  rc;

	M0_ENTRY();

	process = dtm0_service_process__lookup(&dtms->dos_generic, tgt);
	if (process != NULL) {
		*out = process;
		return M0_RC(0);
	}

	M0_ALLOC_PTR(process);
	if (process == NULL)
		return M0_ERR(-ENOMEM);

	rc = dtm0_process_init(process, dtms, tgt);
	if (rc != 0) {
		m0_free(process);
		return M0_ERR(rc);
	}

	*out = process;
	return M0_RC(0);
}

enum drlink_fom_state {
	DRF_INIT = M0_FOM_PHASE_INIT,
	DRF_DONE = M0_FOM_PHASE_FINISH,
	DRF_INITIALISED = M0_FOM_PHASE_NR,
	DRF_LOCKING,
	DRF_LOCKED,
	DRF_TRYING_TO_CONNECT,
	DRF_CONNECTING,
	DRF_READY_TO_SEND,
	DRF_WAITING_FOR_REPLY,
	DRF_FAILED,
	DRF_NR,
};

static struct m0_sm_state_descr drlink_fom_states[] = {
	/* terminal states */
	[DRF_INIT] = {
		.sd_name      = "DRF_INIT",
		.sd_allowed   = M0_BITS(DRF_INITIALISED, DRF_FAILED),
		.sd_flags = M0_SDF_INITIAL,
	},
	[DRF_DONE] = {
		.sd_name      = "DRF_DONE",
		.sd_allowed   = 0,
		.sd_flags = M0_SDF_TERMINAL,
	},
	[DRF_FAILED] = {
		.sd_name      = "DRF_FAILED",
		.sd_allowed   = M0_BITS(DRF_DONE),
		.sd_flags = M0_SDF_TERMINAL,
	},

	/* intermediate states */
#define _ST(name, allowed)             \
	[name] = {                    \
		.sd_name    = #name,  \
		.sd_allowed = allowed \
	}
	_ST(DRF_INITIALISED, M0_BITS(DRF_LOCKING)),
	_ST(DRF_LOCKING,    M0_BITS(DRF_LOCKED)),
	_ST(DRF_LOCKED, M0_BITS(DRF_TRYING_TO_CONNECT)),
	_ST(DRF_TRYING_TO_CONNECT, M0_BITS(DRF_FAILED, DRF_CONNECTING)),
	_ST(DRF_CONNECTING, M0_BITS(DRF_FAILED, DRF_TRYING_TO_CONNECT,
				   DRF_READY_TO_SEND)),
	_ST(DRF_READY_TO_SEND, M0_BITS(DRF_FAILED, DRF_WAITING_FOR_REPLY)),
	_ST(DRF_WAITING_FOR_REPLY, M0_BITS(DRF_FAILED, DRF_DONE)),
#undef _ST
};

static struct m0_sm_trans_descr drlink_fom_trans[] = {
	{ "init-done", M0_FOPH_INIT, DRF_INITIALISED                      },
	{ "init-fail", M0_FOPH_INIT, DRF_FAILED                           },
	{ "lock-wait", DRF_INITIALISED, DRF_LOCKING                       },
	{ "lock-done", DRF_LOCKING, DRF_LOCKED                            },
	{ "first-conn-try", DRF_LOCKED, DRF_TRYING_TO_CONNECT             },
	{ "conn-wait", DRF_TRYING_TO_CONNECT, DRF_CONNECTING              },
	{ "conn-init-fail", DRF_TRYING_TO_CONNECT, DRF_FAILED             },
	{ "conn-fail", DRF_CONNECTING, DRF_FAILED                         },
	{ "conn-retry", DRF_CONNECTING, DRF_TRYING_TO_CONNECT             },
	{ "conn-active", DRF_CONNECTING, DRF_READY_TO_SEND                },
	{ "send-fail", DRF_READY_TO_SEND, DRF_FAILED                      },
	{ "reply-wait", DRF_READY_TO_SEND, DRF_WAITING_FOR_REPLY          },
	{ "reply-fail", DRF_WAITING_FOR_REPLY, DRF_FAILED                 },
	{ "no-reply-reconn", DRF_WAITING_FOR_REPLY, DRF_TRYING_TO_CONNECT },
	{ "delivered", DRF_WAITING_FOR_REPLY, DRF_DONE                    }
};

const static struct m0_sm_conf drlink_fom_conf = {
	.scf_name      = "m0_dtm0_drlink_fom",
	.scf_nr_states = ARRAY_SIZE(drlink_fom_states),
	.scf_state     = drlink_fom_states,
	.scf_trans_nr  = ARRAY_SIZE(drlink_fom_trans),
	.scf_trans     = drlink_fom_trans
};

static void dtm0_rlink_rpc_item_reply_cb(struct m0_rpc_item *item);
const struct m0_rpc_item_ops dtm0_req_fop_rlink_rpc_item_ops = {
        .rio_replied = dtm0_rlink_rpc_item_reply_cb,
};

static struct drlink_fom *item2drlink_fom(struct m0_rpc_item *item)
{
	return m0_rpc_item_to_fop(item)->f_opaque;
}

static void dtm0_rlink_rpc_item_reply_cb(struct m0_rpc_item *item)
{
	struct m0_fop *reply = NULL;
	struct drlink_fom *df = item2drlink_fom(item);

	M0_ENTRY("item=%p", item);

	M0_PRE(item != NULL);
	M0_PRE(M0_IN(m0_fop_opcode(m0_rpc_item_to_fop(item)),
		     (M0_DTM0_REQ_OPCODE)));

	if (m0_rpc_item_error(item) == 0) {
		reply = m0_rpc_item_to_fop(item->ri_reply);
		M0_ASSERT(M0_IN(m0_fop_opcode(reply), (M0_DTM0_REP_OPCODE)));
	}

	m0_fom_wakeup(&df->df_gen);

	M0_LEAVE("reply=%p", reply);
}

static int dtm0_process_rlink_reinit(struct dtm0_process *proc,
				     struct drlink_fom   *df)
{
	struct m0_rpc_machine *mach = df->df_rfop->f_item.ri_rmachine;
	const int max_in_flight = DTM0_MAX_RPCS_IN_FLIGHT;

	if (!M0_IS0(&proc->dop_rlink)) {
		m0_rpc_link_fini(&proc->dop_rlink);
		M0_SET0(&proc->dop_rlink);
	}

	return m0_rpc_link_init(&proc->dop_rlink, mach, &proc->dop_rserv_fid,
				proc->dop_rep, max_in_flight);
}

static int dtm0_process_rlink_send(struct dtm0_process *proc,
				   struct drlink_fom *df)
{
	struct m0_fop          *fop = df->df_rfop;
	struct m0_rpc_session  *session = &proc->dop_rlink.rlk_sess;
	struct m0_rpc_item     *item = &fop->f_item;

	item->ri_ops      = &dtm0_req_fop_rpc_item_ops;
	item->ri_session  = session;
	item->ri_prio     = M0_RPC_ITEM_PRIO_MID;
	item->ri_deadline = M0_TIME_IMMEDIATELY;

	return m0_rpc_post(item);
}

static bool dtm0_process_ha_is_alive(struct dtm0_process *proc)
{
	(void) proc;
	/* TODO: check conf_obj state */
	return true;
}

#define F M0_CO_FRAME_DATA
static void drlink_coro_fom_tick(struct m0_co_context *context, int *prc)
{
	M0_CO_REENTER(context,
		      struct m0_long_lock_link   llink;
		      struct m0_long_lock_addb2  llock_addb2;
		      struct dtm0_process       *proc;
		      struct drlink_fom         *drf;
		      struct m0_fom             *fom;
		      int                        rc;
		      bool                       delivered;
		      struct m0_rpc_item        *item;
		      );

	F(rc) = 0;
	F(delivered) = false;
	F(drf) = M0_AMB(F(drf), context, df_co);
	F(fom) = &F(drf)->df_gen;
	F(item) = &F(drf)->df_rfop->f_item;

	m0_mutex_lock(&F(drf)->df_svc->dos_generic.rs_mutex);
	F(rc) = find_or_add(F(drf)->df_svc, &F(drf)->df_tgt, &F(proc));
	/* Safety: assume that processes cannot be evicted */
	m0_mutex_unlock(&F(drf)->df_svc->dos_generic.rs_mutex);

	if (F(rc) != 0)
		goto out;

	m0_long_lock_link_init(&F(llink), F(fom), &F(llock_addb2));
	m0_fom_phase_set(F(fom), DRF_INITIALISED);

	M0_CO_FUN(context, co_long_write_lock(context,
					      &F(proc)->dop_llock,
					      &F(llink),
					      DRF_LOCKING));

	while (!F(delivered)) {
		while (!m0_rpc_link_is_connected(&F(proc)->dop_rlink)) {
			m0_fom_phase_set(F(fom), DRF_TRYING_TO_CONNECT);
			F(rc) = dtm0_process_rlink_reinit(F(proc), F(drf));
			if (F(rc) != 0)
				goto unlock;
			M0_CO_FUN(context,
				  co_rpc_link_connect(context,
						      &F(proc)->dop_rlink,
						      F(fom), DRF_CONNECTING));
			if (!dtm0_process_ha_is_alive(F(proc)))
				goto unlock;
			if (F(proc)->dop_rlink.rlk_rc != 0) {
				/* TODO: figure out something better. */
				(void) M0_ERR(F(proc)->dop_rlink.rlk_rc);
			}
		}

		m0_fom_phase_set(F(fom), DRF_READY_TO_SEND);
		F(rc) = dtm0_process_rlink_send(F(proc), F(drf));
		if (F(rc) != 0)
			goto unlock;

		m0_fom_phase_set(F(fom), DRF_WAITING_FOR_REPLY);
		M0_CO_YIELD(context, -EINTR);

		F(delivered) = m0_rpc_item_error(F(item)) == 0;
		/* TODO: Add more showstoppers here (ENOBUFS or something). */
		if (M0_IN(m0_rpc_item_error(F(item)), (-ENOMEM))) {
			F(rc) = m0_rpc_item_error(F(item));
			goto unlock;
		}
	}

	m0_fom_phase_set(F(fom), DRF_DONE);

unlock:
	m0_long_write_unlock(&F(proc)->dop_llock, &F(llink));
	m0_long_lock_link_fini(&F(llink));
out:
	*prc = F(rc);
	return;
}

static int co_ret2fom_ret(int rc)
{
	switch (rc) {
	case 0:
		return 0;
	case -EAGAIN:
		return M0_FSO_AGAIN;
	case -EINTR:
		return M0_FSO_WAIT;
	default:
		M0_IMPOSSIBLE();
	}
}

static int drlink_fom_tick(struct m0_fom *fom)
{
	struct m0_co_context *co;
	int                   phase_rc;
	int                   outcome;

	co = &fom2drlink_fom(fom)->df_co;

	M0_CO_START(co);
	drlink_coro_fom_tick(co, &phase_rc);
	outcome = co_ret2fom_ret(M0_CO_END(co));

	if (outcome == 0 && phase_rc != 0)
		m0_fom_phase_move(fom, phase_rc, M0_FOPH_FAILURE);

	return outcome;
}

M0_INTERNAL int m0_dtm0_req_post(struct m0_dtm0_service    *svc,
				 const struct dtm0_req_fop *req,
				 const struct m0_fid       *tgt)
{
	int                  rc;
	struct drlink_fom   *fom;

	M0_ENTRY();

	M0_ALLOC_PTR(fom);
	if (fom == NULL)
		return M0_ERR(-ENOMEM);

	rc = drlink_fom_init(fom, svc, tgt, req);
	if (rc != 0) {
		m0_free(fom);
		return M0_ERR(rc);
	}

	m0_fom_queue(&fom->df_gen);

	return M0_RC(rc);
}

#else /* !defined(__KERNEL__) */
static int m0_dtm0_rpc_link_mod_init(void)
{
	M0_IMPOSSIBLE();
	return 0;
}

static void m0_dtm0_rpc_link_mod_fini(void)
{
	M0_IMPOSSIBLE();
}
M0_INTERNAL int m0_dtm0_post_msg(struct m0_dtm0_service    *svc,
				 const struct dtm0_req_fop *req,
				 const struct m0_fid       *tgt)
{
	(void) svc;
	(void) req;
	(void) tgt;
	M0_IMPOSSIBLE();
	return 0;
}
#endif /* !defined(__KERNEL__) */


/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
