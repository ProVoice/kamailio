/*
 * sl module - stateless reply
 *
 * Copyright (C) 2001-2003 FhG Fokus
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Kamailio is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * Kamailio is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/**
 * @file
 * @brief SL :: module definitions
 *
 * @ingroup sl
 * Module: @ref sl
 */

/*!
 * @defgroup sl SL :: The SER SL Module
 *
 * The SL module allows SER to act as a stateless UA server and
 * generate replies to SIP requests without keeping state. That is beneficial
 * in many scenarios, in which you wish not to burden server's memory and scale
 * well.
 */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../core/sr_module.h"
#include "../../core/dprint.h"
#include "../../core/error.h"
#include "../../core/ut.h"
#include "../../core/data_lump.h"
#include "../../core/mod_fix.h"
#include "../../core/script_cb.h"
#include "../../core/mem/mem.h"
#include "../../core/kemi.h"

#include "../../modules/tm/tm_load.h"

#include "sl_stats.h"
#include "sl_funcs.h"
#include "sl.h"

MODULE_VERSION

static int default_code = 500;
static str default_reason = STR_STATIC_INIT("Internal Server Error");

static int sl_bind_tm = 1;
static struct tm_binds tmb;

str _sl_event_callback_fl_ack = STR_NULL;
str _sl_event_callback_lres_sent = STR_NULL;

static int w_sl_send_reply(struct sip_msg *msg, char *str1, char *str2);
static int w_send_reply(struct sip_msg *msg, char *str1, char *str2);
static int w_send_reply_mode(
		struct sip_msg *msg, char *str1, char *str2, char *str3);
static int w_send_reply_error(sip_msg_t *msg, char *str1, char *str2);
static int w_sl_reply_error(struct sip_msg *msg, char *str1, char *str2);
static int w_sl_forward_reply0(sip_msg_t *msg, char *str1, char *str2);
static int w_sl_forward_reply1(sip_msg_t *msg, char *str1, char *str2);
static int w_sl_forward_reply2(sip_msg_t *msg, char *str1, char *str2);
static int bind_sl(sl_api_t *api);
static int mod_init(void);
static int child_init(int rank);
static void mod_destroy();
static int fixup_sl_reply(void **param, int param_no);
static int fixup_sl_reply_mode(void **param, int param_no);
static int fixup_free_sl_reply_mode(void **param, int param_no);

static int pv_get_ltt(sip_msg_t *msg, pv_param_t *param, pv_value_t *res);
static int pv_parse_ltt_name(pv_spec_p sp, str *in);


/* clang-format off */
static pv_export_t mod_pvs[] = {
	{{"ltt", (sizeof("ltt") - 1)}, PVT_OTHER, pv_get_ltt, 0,
				pv_parse_ltt_name, 0, 0, 0},

	{{0, 0}, 0, 0, 0, 0, 0, 0, 0}
};

static cmd_export_t cmds[] = {
	{"sl_send_reply", w_sl_send_reply, 2, fixup_sl_reply, fixup_free_fparam_all, REQUEST_ROUTE},
	{"sl_reply", w_sl_send_reply, 2, fixup_sl_reply, fixup_free_fparam_all, REQUEST_ROUTE},
	{"send_reply", w_send_reply, 2, fixup_sl_reply, fixup_free_fparam_all,
			REQUEST_ROUTE | ONREPLY_ROUTE | FAILURE_ROUTE},
	{"send_reply_mode", (cmd_function)w_send_reply_mode, 3,	fixup_sl_reply_mode, fixup_free_sl_reply_mode,
			REQUEST_ROUTE | ONREPLY_ROUTE | FAILURE_ROUTE},
	{"send_reply_error", w_send_reply_error, 0, 0, 0,
			REQUEST_ROUTE | ONREPLY_ROUTE | FAILURE_ROUTE},
	{"sl_reply_error", w_sl_reply_error, 0, 0, 0, REQUEST_ROUTE},
	{"sl_forward_reply", w_sl_forward_reply0, 0, 0, 0, ONREPLY_ROUTE},
	{"sl_forward_reply", w_sl_forward_reply1, 1, fixup_spve_all, fixup_free_spve_all, ONREPLY_ROUTE},
	{"sl_forward_reply", w_sl_forward_reply2, 2, fixup_spve_all, fixup_free_spve_all, ONREPLY_ROUTE},
	{"bind_sl", (cmd_function)bind_sl, 0, 0, 0, 0},
	{0, 0, 0, 0, 0, 0}
};


/*
 * Exported parameters
 */
static param_export_t params[] = {
	{"default_code", PARAM_INT, &default_code},
	{"default_reason", PARAM_STR, &default_reason},
	{"bind_tm", PARAM_INT, &sl_bind_tm},
	{"rich_redirect", PARAM_INT, &sl_rich_redirect},
	{"event_callback_fl_ack", PARAM_STR, &_sl_event_callback_fl_ack},
	{"event_callback_lres_sent", PARAM_STR, &_sl_event_callback_lres_sent},

	{0, 0, 0}
};


#ifdef STATIC_SL
struct module_exports sl_exports = {
#else
struct module_exports exports = {
#endif
	"sl",			 /* module name */
	DEFAULT_DLFLAGS, /* dlopen flags */
	cmds,			 /* cmd (cfg function) exports */
	params,			 /* param exports */
	sl_rpc,			 /* RPC method exports */
	mod_pvs,		 /* pv exports */
	0,				 /* response handling function */
	mod_init,		 /* module init function */
	child_init,		 /* per-child init function */
	mod_destroy		 /* module destroy function */
};
/* clang-format on */


static int mod_init(void)
{
	if(init_sl_stats() < 0) {
		ERR("init_sl_stats failed\n");
		return -1;
	}
	if(sl_register_kstats() < 0) {
		ERR("init k stats failed\n");
		return -1;
	}

	/* if SL loaded, filter ACKs on beginning */
	if(register_script_cb(sl_filter_ACK, PRE_SCRIPT_CB | REQUEST_CB, 0) < 0) {
		ERR("Failed to install SCRIPT callback\n");
		return -1;
	}
	if(sl_startup() < 0) {
		ERR("Failed to do startup tasks\n");
		return -1;
	}

	memset(&tmb, 0, sizeof(struct tm_binds));
	if(sl_bind_tm != 0) {
		if(load_tm_api(&tmb) == -1) {
			LM_INFO("could not bind tm module - only stateless mode"
					" available during modules initialization\n");
		}
	}

	sl_lookup_event_routes();

	return 0;
}

static int child_init(int rank)
{
	if(rank == PROC_INIT) {
		if(init_sl_stats_child() < 0) {
			ERR("init_sl_stats_child failed\n");
			return -1;
		}
		if(sl_bind_tm != 0 && tmb.register_tmcb == 0) {
			if(load_tm_api(&tmb) == -1) {
				LM_INFO("could not bind tm module - only stateless mode"
						" available during runtime\n");
				sl_bind_tm = 0;
			}
		}
	}
	return 0;
}


static void mod_destroy()
{
	sl_stats_destroy();
	sl_shutdown();
}


/**
 * @brief Small wrapper around sl_send_reply
 *
 * Wrapper around sl_send_reply() which accepts parameters that include
 * config variables
 *
 */
static int w_sl_send_reply(struct sip_msg *msg, char *p1, char *p2)
{
	int code, ret;
	str reason;
	char *r;

	if(get_int_fparam(&code, msg, (fparam_t *)p1) < 0) {
		code = default_code;
	}

	if(get_str_fparam(&reason, msg, (fparam_t *)p2) < 0) {
		reason = default_reason;
	}

	if(reason.s[reason.len - 1] == '\0') {
		r = reason.s;
	} else {
		r = as_asciiz(&reason);
		if(r == NULL)
			r = default_reason.s;
	}
	ret = sl_send_reply(msg, code, r);
	if((r != reason.s) && (r != default_reason.s))
		pkg_free(r);

	return ret;
}


/**
 * @brief Small wrapper around sl_reply_error
 */
static int w_sl_reply_error(struct sip_msg *msg, char *str, char *str2)
{
	return sl_reply_error(msg);
}

/**
 * @brief send stateful reply if transaction was created
 *
 * Check if transaction was created for respective SIP request and reply
 * in stateful mode, otherwise send stateless reply
 *
 * @param msg - SIP message structure
 * @param code - reply status code
 * @param reason - reply reason phrase
 * @return 1 for success and -1 for failure
 */
int send_reply(struct sip_msg *msg, int code, str *reason)
{
	char *r = NULL;
	struct cell *t;
	int ret = 1;

	if(msg->msg_flags & FL_MSG_NOREPLY) {
		LM_INFO("message marked with no-reply flag\n");
		return -2;
	}

	if(reason->s[reason->len - 1] == '\0') {
		r = reason->s;
	} else {
		r = as_asciiz(reason);
		if(r == NULL) {
			LM_ERR("no pkg for reason phrase\n");
			return -1;
		}
	}

	if(sl_bind_tm != 0 && tmb.t_gett != 0) {
		t = tmb.t_gett();
		if(t != NULL && t != T_UNDEFINED) {
			if(tmb.t_reply(msg, code, r) < 0) {
				LM_ERR("failed to reply stateful (tm)\n");
				goto error;
			}
			LM_DBG("reply in stateful mode (tm)\n");
			goto done;
		}
	}

	if(msg->first_line.type == SIP_REPLY)
		goto error;

	LM_DBG("reply in stateless mode (sl)\n");
	ret = sl_send_reply(msg, code, r);

done:
	if(r != reason->s)
		pkg_free(r);
	return ret;

error:
	if(r != reason->s)
		pkg_free(r);
	return -1;
}

/**
 * @brief Small wrapper around send_reply
 */
static int w_send_reply(struct sip_msg *msg, char *p1, char *p2)
{
	int code;
	str reason;

	if(get_int_fparam(&code, msg, (fparam_t *)p1) < 0) {
		code = default_code;
	}

	if(get_str_fparam(&reason, msg, (fparam_t *)p2) < 0) {
		reason = default_reason;
	}

	return send_reply(msg, code, &reason);
}

/**
 *
 */
static int ki_send_reply_error(sip_msg_t *msg)
{
	int ret;

	if(msg->msg_flags & FL_FINAL_REPLY) {
		LM_INFO("message marked with final-reply flag\n");
		return -2;
	}
	if(msg->msg_flags & FL_DELAYED_REPLY) {
		LM_INFO("message marked with delayed-reply flag\n");
		return -3;
	}

	if(sl_bind_tm != 0 && tmb.t_reply_error != NULL) {
		ret = tmb.t_reply_error(msg);
		if(ret > 0) {
			return ret;
		}
	}
	return sl_reply_error(msg);
}

/**
 *
 */
static int w_send_reply_error(sip_msg_t *msg, char *p1, char *p2)
{
	return ki_send_reply_error(msg);
}

/**
 * @brief send stateful reply if transaction was created
 *
 * Check if transaction was created for respective SIP request and reply
 * in stateful mode, otherwise send stateless reply
 *
 * @param msg - SIP message structure
 * @param code - reply status code
 * @param reason - reply reason phrase
 * @param mode - execution mode
 * @return 1 for success and -1 for failure
 */
int ki_send_reply_mode(struct sip_msg *msg, int code, str *reason, int mode)
{
	if(mode & SET_RPL_NO_CONNECT_T) {
		msg->rpl_send_flags.f |= SND_F_FORCE_CON_REUSE;
	} else if(mode & SET_RPL_CLOSE_T) {
		msg->rpl_send_flags.f |= SND_F_CON_CLOSE;
	}

	return send_reply(msg, code, reason);
}

/**
 * @brief Small wrapper around send_reply
 */
static int w_send_reply_mode(struct sip_msg *msg, char *p1, char *p2, char *p3)
{
	int code;
	str reason;
	int mode = 0;

	if(get_int_fparam(&code, msg, (fparam_t *)p1) < 0) {
		code = default_code;
	}

	if(get_str_fparam(&reason, msg, (fparam_t *)p2) < 0) {
		reason = default_reason;
	}

	if(get_int_fparam(&mode, msg, (fparam_t *)p3) < 0) {
		mode = 0;
	}

	return ki_send_reply_mode(msg, code, &reason, mode);
}

/**
 * @brief store To-tag value in totag parameter
 */
int get_reply_totag(struct sip_msg *msg, str *totag)
{
	struct cell *t;
	if(msg == NULL || totag == NULL)
		return -1;
	if(sl_bind_tm != 0 && tmb.t_gett != 0) {
		t = tmb.t_gett();
		if(t != NULL && t != T_UNDEFINED) {
			if(tmb.t_get_reply_totag(msg, totag) < 0) {
				LM_ERR("failed to get totag (tm)\n");
				return -1;
			}
			LM_DBG("totag stateful mode (tm)\n");
			return 1;
		}
	}

	LM_DBG("totag stateless mode (sl)\n");
	return sl_get_reply_totag(msg, totag);
}


/**
 * @brief fixup for SL reply config file functions
 */
static int fixup_sl_reply(void **param, int param_no)
{
	if(param_no == 1) {
		return fixup_var_int_12(param, 1);
	} else if(param_no == 2) {
		return fixup_var_pve_str_12(param, 2);
	}
	return 0;
}

/**
 * @brief fixup for SL reply mode config file functions
 */
static int fixup_sl_reply_mode(void **param, int param_no)
{
	if(param_no == 1) {
		return fixup_var_int_12(param, 1);
	} else if(param_no == 2) {
		return fixup_var_pve_str_12(param, 2);
	} else if(param_no == 3) {
		return fixup_var_int_12(param, 1);
	}
	return 0;
}

static int fixup_free_sl_reply_mode(void **param, int param_no)
{
	if(param_no == 1 || param_no == 3) {
		fixup_free_fparam_all(param, 1);
	} else {
		fixup_free_fparam_all(param, param_no);
	}
	return 0;
}

/**
 * @brief forward SIP reply statelessly with different code and reason text
 */
static int w_sl_forward_reply(sip_msg_t *msg, str *code, str *reason)
{
	char oldscode[3];
	int oldncode;
	int ret;
	struct lump *ldel = NULL;
	struct lump *ladd = NULL;
	char *rbuf;

	if(msg->first_line.type != SIP_REPLY) {
		LM_ERR("invalid SIP message type\n");
		return -1;
	}
	if(code != NULL && code->len > 0) {
		if(code->len != 3) {
			LM_ERR("invalid reply code value %.*s\n", code->len, code->s);
			return -1;
		}
		if(msg->first_line.u.reply.status.s[0] != code->s[0]) {
			LM_ERR("reply code class cannot be changed\n");
			return -1;
		}
		if(code->s[1] < '0' || code->s[1] > '9' || code->s[2] < '0'
				|| code->s[2] > '9') {
			LM_ERR("invalid reply code value %.*s!\n", code->len, code->s);
			return -1;
		}
	}
	/* backup old values */
	oldscode[0] = msg->first_line.u.reply.status.s[0];
	oldscode[1] = msg->first_line.u.reply.status.s[1];
	oldscode[2] = msg->first_line.u.reply.status.s[2];
	oldncode = msg->first_line.u.reply.statuscode;
	if(code != NULL && code->len > 0) {
		/* update status code directly in msg buffer */
		msg->first_line.u.reply.statuscode = (code->s[0] - '0') * 100
											 + (code->s[1] - '0') * 10
											 + code->s[2] - '0';
		msg->first_line.u.reply.status.s[0] = code->s[0];
		msg->first_line.u.reply.status.s[1] = code->s[1];
		msg->first_line.u.reply.status.s[2] = code->s[2];
	}
	if(reason != NULL && reason->len > 0) {
		ldel = del_lump(msg, msg->first_line.u.reply.reason.s - msg->buf,
				msg->first_line.u.reply.reason.len, 0);
		if(ldel == NULL) {
			LM_ERR("failed to add del lump\n");
			ret = -1;
			goto restore;
		}
		rbuf = (char *)pkg_malloc(reason->len);
		if(rbuf == NULL) {
			PKG_MEM_ERROR;
			ret = -1;
			goto restore;
		}
		memcpy(rbuf, reason->s, reason->len);
		ladd = insert_new_lump_after(ldel, rbuf, reason->len, 0);
		if(ladd == 0) {
			LM_ERR("failed to add reason lump: %.*s\n", reason->len, reason->s);
			pkg_free(rbuf);
			ret = -1;
			goto restore;
		}
	}
	ret = forward_reply_nocb(msg);
restore:
	if(reason != NULL && reason->len > 0) {
		if(ldel != NULL) {
			remove_lump(msg, ldel);
			/* ladd is liked in the 'after' list inside ldel,
			 * destroyed together, no need for its own remove operation */
		}
	}
	if(code != NULL && code->len > 0) {
		msg->first_line.u.reply.statuscode = oldncode;
		msg->first_line.u.reply.status.s[0] = oldscode[0];
		msg->first_line.u.reply.status.s[1] = oldscode[1];
		msg->first_line.u.reply.status.s[2] = oldscode[2];
	}
	return (ret == 0) ? 1 : ret;
}

/**
 * @brief forward SIP reply statelessly
 */
static int w_sl_forward_reply0(sip_msg_t *msg, char *str1, char *str2)
{
	return w_sl_forward_reply(msg, NULL, NULL);
}

/**
 * @brief forward SIP reply statelessly with a new code
 */
static int w_sl_forward_reply1(sip_msg_t *msg, char *str1, char *str2)
{
	str code;
	if(fixup_get_svalue(msg, (gparam_t *)str1, &code) < 0) {
		LM_ERR("cannot get the reply code parameter value\n");
		return -1;
	}
	return w_sl_forward_reply(msg, &code, NULL);
}

/**
 * @brief forward SIP reply statelessly with new code and reason text
 */
static int w_sl_forward_reply2(sip_msg_t *msg, char *str1, char *str2)
{
	str code;
	str reason;
	if(fixup_get_svalue(msg, (gparam_t *)str1, &code) < 0) {
		LM_ERR("cannot get the reply code parameter value\n");
		return -1;
	}
	if(fixup_get_svalue(msg, (gparam_t *)str2, &reason) < 0) {
		LM_ERR("cannot get the reply reason parameter value\n");
		return -1;
	}
	return w_sl_forward_reply(msg, &code, &reason);
}

/**
 *
 */
static int pv_get_ltt(sip_msg_t *msg, pv_param_t *param, pv_value_t *res)
{
	str ttag = STR_NULL;
	tm_cell_t *t = NULL;

	if(msg == NULL)
		return pv_get_null(msg, param, res);

	if(param == NULL)
		return pv_get_null(msg, param, res);

	switch(param->pvn.u.isname.name.n) {
		case 0: /* mixed */
			if(get_reply_totag(msg, &ttag) < 0) {
				return pv_get_null(msg, param, res);
			}
			return pv_get_strval(msg, param, res, &ttag);
		case 1: /* stateless */
			if(sl_get_reply_totag(msg, &ttag) < 0) {
				return pv_get_null(msg, param, res);
			}
			return pv_get_strval(msg, param, res, &ttag);
		case 2: /* transaction stateful */
			if(sl_bind_tm == 0 || tmb.t_gett == 0) {
				return pv_get_null(msg, param, res);
			}

			t = tmb.t_gett();
			if(t == NULL || t == T_UNDEFINED) {
				return pv_get_null(msg, param, res);
			}
			if(tmb.t_get_reply_totag(msg, &ttag) < 0) {
				return pv_get_null(msg, param, res);
			}
			return pv_get_strval(msg, param, res, &ttag);
		default:
			return pv_get_null(msg, param, res);
	}
}

/**
 *
 */
static int pv_parse_ltt_name(pv_spec_p sp, str *in)
{
	if(sp == NULL || in == NULL || in->len <= 0)
		return -1;

	switch(in->len) {
		case 1:
			if(strncmp(in->s, "x", 1) == 0) {
				sp->pvp.pvn.u.isname.name.n = 0;
			} else if(strncmp(in->s, "s", 1) == 0) {
				sp->pvp.pvn.u.isname.name.n = 1;
			} else if(strncmp(in->s, "t", 1) == 0) {
				sp->pvp.pvn.u.isname.name.n = 2;
			} else {
				goto error;
			}
			break;
		default:
			goto error;
	}
	sp->pvp.pvn.type = PV_NAME_INTSTR;
	sp->pvp.pvn.u.isname.type = 0;

	return 0;

error:
	LM_ERR("unknown PV ltt key: %.*s\n", in->len, in->s);
	return -1;
}


/**
 * @brief bind functions to SL API structure
 */
static int bind_sl(sl_api_t *api)
{
	if(!api) {
		ERR("Invalid parameter value\n");
		return -1;
	}
	api->zreply = sl_send_reply;
	api->sreply = sl_send_reply_str;
	api->dreply = sl_send_reply_dlg;
	api->freply = send_reply;
	api->get_reply_totag = get_reply_totag;
	api->register_cb = sl_register_callback;

	return 0;
}

/**
 *
 */
/* clang-format off */
static sr_kemi_t sl_kemi_exports[] = {
	{ str_init("sl"), str_init("sl_send_reply"),
		SR_KEMIP_INT, sl_send_reply_str,
		{ SR_KEMIP_INT, SR_KEMIP_STR, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("sl"), str_init("send_reply"),
		SR_KEMIP_INT, send_reply,
		{ SR_KEMIP_INT, SR_KEMIP_STR, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("sl"), str_init("send_reply_mode"),
		SR_KEMIP_INT, ki_send_reply_mode,
		{ SR_KEMIP_INT, SR_KEMIP_STR, SR_KEMIP_INT,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("sl"), str_init("sl_reply_error"),
		SR_KEMIP_INT, sl_reply_error,
		{ SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("sl"), str_init("send_reply_error"),
		SR_KEMIP_INT, ki_send_reply_error,
		{ SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},
	{ str_init("sl"), str_init("sl_forward_reply"),
		SR_KEMIP_INT, w_sl_forward_reply,
		{ SR_KEMIP_STR, SR_KEMIP_STR, SR_KEMIP_NONE,
			SR_KEMIP_NONE, SR_KEMIP_NONE, SR_KEMIP_NONE }
	},

	{ {0, 0}, {0, 0}, 0, NULL, { 0, 0, 0, 0, 0, 0 } }
};
/* clang-format on */

int mod_register(char *path, int *dlflags, void *p1, void *p2)
{
	sr_kemi_modules_add(sl_kemi_exports);
	return 0;
}
