/* GSM Subscriber Update Protocol */

/* (C) 2015 by Ivan Klyuchnikov <kluchnikovi@gmail.com>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <openbsc/gsm_sup.h>
#include <openbsc/gsm_subscriber.h>
#include <openbsc/gsm_04_08.h>
#include <openbsc/debug.h>
#include <openbsc/db.h>
#include <openbsc/chan_alloc.h>
#include <openbsc/gsm_04_08_gprs.h>
#include <openbsc/gprs_gsup_messages.h>
#include <openbsc/gprs_gsup_client.h>
#include <openbsc/osmo_msc.h>
#include <openbsc/gprs_utils.h>
#include <openbsc/ussd.h>

enum {
    FMAP_MSISDN        = 0x80
};

static int subscr_uss_message(struct msgb *msg,
			      struct ss_request *req,
			      const char* extention)
{
	size_t bcd_len = 0;
	uint8_t *gsup_indicator;

	gsup_indicator = msgb_put(msg, 4);

	/* First byte should always be GPRS_GSUP_MSGT_MAP */
	gsup_indicator[0] = GPRS_GSUP_MSGT_MAP;
	gsup_indicator[1] = req->message_type;
	/* TODO ADD tid */
	gsup_indicator[2] = req->component_type;
	gsup_indicator[3] = req->transaction_id;

	/* invokeId */
	msgb_tlv_put(msg, GSM0480_COMPIDTAG_INVOKE_ID, 1, &req->invoke_id);

	/* opCode */
	if (req->opcode != 0) {
		msgb_tlv_put(msg, GSM0480_OPERATION_CODE, 1, &req->opcode);
	}

	if (req->ussd_text_len > 0) {
		//msgb_tlv_put(msg, ASN1_OCTET_STRING_TAG, 1, &req->ussd_text_language);
		msgb_tlv_put(msg, ASN1_OCTET_STRING_TAG, req->ussd_text_len, req->ussd_text);
	}

	if (extention) {
		uint8_t bcd_buf[32];
		bcd_len = gsm48_encode_bcd_number(bcd_buf, sizeof(bcd_buf), 0,
						  extention);
		msgb_tlv_put(msg, FMAP_MSISDN, bcd_len - 1, &bcd_buf[1]);
	}

	/* fill actual length */
	gsup_indicator[3] = 3 + 3 + (req->ussd_text_len + 2) + (bcd_len + 2);;

	/* wrap with GSM0480_CTYPE_INVOKE */
	// gsm0480_wrap_invoke(msg, req->opcode, invoke_id);
	// gsup_indicator = msgb_push(msgb, 1);
	// gsup_indicator[0] = GPRS_GSUP_MSGT_MAP;
	return 0;
}


int subscr_tx_uss_message(struct ss_request *req,
			  struct gsm_subscriber *subscr)
{
	struct msgb *msg = gprs_gsup_msgb_alloc();
	if (!msg)
		return -ENOMEM;

	//GSM0480_OP_CODE_PROCESS_USS_REQ
	subscr_uss_message(msg, req, subscr->extension);

	return gprs_gsup_client_send(subscr->group->net->ussd_sup_client, msg);
}


static int rx_uss_message_parse(struct ss_request *ss,
				const uint8_t* data,
				size_t len,
				char* extention,
				size_t extention_len)
{
	const uint8_t* const_data = data;

	if (len < 1 + 2 + 3 + 3)
		return -1;

	/* skip GPRS_GSUP_MSGT_MAP */
	ss->message_type = *(++const_data);
	ss->component_type = *(++const_data);
	ss->transaction_id = *(++const_data);
	const_data += 1;

	//
	if (*const_data != GSM0480_COMPIDTAG_INVOKE_ID) {
		return -1;
	}
	const_data += 2;
	ss->invoke_id = *const_data;
	const_data++;

	//
	if (*const_data != GSM0480_OPERATION_CODE) {
		return -1;
	}
	const_data += 2;
	ss->opcode = *const_data;
	const_data++;


	while (const_data - data < len) {
		uint8_t len;
		switch (*const_data) {
		case ASN1_OCTET_STRING_TAG:
			len = *(++const_data);
			strncpy((char*)ss->ussd_text,
				(const char*)++const_data,
				(len > MAX_LEN_USSD_STRING) ? MAX_LEN_USSD_STRING : len);
			const_data += len;
			break;

		case FMAP_MSISDN:
			len = *(++const_data);
			gsm48_decode_bcd_number(extention,
						extention_len,
						const_data,
						0);
			const_data += len + 1;
			break;
		default:
			DEBUGP(DMM, "Unknown code: %d\n", *const_data);
			return -1;
		}
	}

	return 0;
}

static int rx_uss_message(const uint8_t* data, size_t len)
{
	char extention[32] = {0};
	struct ss_request ss;
	memset(&ss, 0, sizeof(ss));

	if (rx_uss_message_parse(&ss, data, len, extention, sizeof(extention))) {
		LOGP(DSUP, LOGL_ERROR, "Can't parse uss message\n");
		return -1;
	}

	LOGP(DSUP, LOGL_ERROR, "Got invoke_id=0x%02x opcode=0x%02x facility=0x%02x text=%s\n",
	     ss.invoke_id, ss.opcode, ss.component_type, ss.ussd_text);

	return on_ussd_response(&ss, extention);
}


static int subscr_tx_sup_message(struct gprs_gsup_client *sup_client,
								 struct gsm_subscriber *subscr,
								 struct gprs_gsup_message *gsup_msg)
{
	struct msgb *msg = gprs_gsup_msgb_alloc();

	if (strlen(gsup_msg->imsi) == 0 && subscr)
		strncpy(gsup_msg->imsi, subscr->imsi, sizeof(gsup_msg->imsi) - 1);

	gprs_gsup_encode(msg, gsup_msg);

	LOGGSUBSCRP(LOGL_INFO, subscr,
		    "Sending SUP, will send: %s\n", msgb_hexdump(msg));

	if (!sup_client) {
		msgb_free(msg);
		return -ENOTSUP;
	}

	return gprs_gsup_client_send(sup_client, msg);
}

int subscr_query_auth_info(struct gsm_subscriber *subscr)
{
	struct gprs_gsup_message gsup_msg = {0};

	LOGGSUBSCRP(LOGL_INFO, subscr,
		"subscriber auth info is not available\n");

	gsup_msg.message_type = GPRS_GSUP_MSGT_SEND_AUTH_INFO_REQUEST;
	return subscr_tx_sup_message(subscr->group->net->hlr_sup_client, subscr, &gsup_msg);
}

int subscr_location_update(struct gsm_subscriber *subscr)
{
	struct gprs_gsup_message gsup_msg = {0};

	LOGGSUBSCRP(LOGL_INFO, subscr,
		"subscriber data is not available\n");

	gsup_msg.message_type = GPRS_GSUP_MSGT_UPDATE_LOCATION_REQUEST;
	return subscr_tx_sup_message(subscr->group->net->hlr_sup_client, subscr, &gsup_msg);
}

static int subscr_tx_sup_error_reply(struct gprs_gsup_client *sup_client,
									 struct gsm_subscriber *subscr,
									 struct gprs_gsup_message *gsup_orig,
									 enum gsm48_gmm_cause cause)
{
	struct gprs_gsup_message gsup_reply = {0};

	strncpy(gsup_reply.imsi, gsup_orig->imsi, sizeof(gsup_reply.imsi) - 1);
	gsup_reply.cause = cause;
	gsup_reply.message_type =
		GPRS_GSUP_TO_MSGT_ERROR(gsup_orig->message_type);

	return subscr_tx_sup_message(sup_client, subscr, &gsup_reply);
}

static int subscr_handle_sup_auth_res(struct gprs_gsup_client *sup_client,
									   struct gsm_subscriber *subscr,
									   struct gprs_gsup_message *gsup_msg)
{
	struct gsm_subscriber_connection *conn = connection_for_subscr(subscr);
	struct gsm_security_operation *op;


	LOGGSUBSCRP(LOGL_INFO, subscr,
		"Got SendAuthenticationInfoResult, num_auth_tuples = %zu\n",
		gsup_msg->num_auth_tuples);

	if (gsup_msg->num_auth_tuples > 0) {
		op = conn->sec_operation;
		memcpy(&op->atuple, gsup_msg->auth_tuples, sizeof(struct gsm_auth_tuple));
		db_sync_lastauthtuple_for_subscr(&op->atuple, subscr);
		gsm48_tx_mm_auth_req(conn, op->atuple.rand, op->atuple.key_seq);
	}

	return 0;
}

static int subscr_handle_sup_upd_loc_res(struct gsm_subscriber *subscr,
									struct gprs_gsup_message *gsup_msg)
{
	uint8_t msisdn_lv[10];

	if (gsup_msg->msisdn_enc) {
		if (gsup_msg->msisdn_enc_len > sizeof(msisdn_lv) - 1) {
			LOGP(DSUP, LOGL_ERROR, "MSISDN too long (%zu)\n",
				gsup_msg->msisdn_enc_len);
			return -1;
		} else {
			msisdn_lv[0] = gsup_msg->msisdn_enc_len;
			memcpy(msisdn_lv+1, gsup_msg->msisdn_enc,
				gsup_msg->msisdn_enc_len);
			gsm48_decode_bcd_number(subscr->extension, sizeof(subscr->extension),
																	msisdn_lv,0);
		}
	}

	db_sync_subscriber(subscr);

	struct gsm_subscriber_connection *conn = connection_for_subscr(subscr);

	if (conn->loc_operation)
		conn->loc_operation->waiting_for_remote_accept = 0;

	gsm0408_authorize(conn,NULL);

	return 0;
}

static int check_cause(int cause)
{
	switch (cause) {
	case GMM_CAUSE_IMSI_UNKNOWN ... GMM_CAUSE_ILLEGAL_ME:
	case GMM_CAUSE_GPRS_NOTALLOWED ... GMM_CAUSE_NO_GPRS_PLMN:
		return EACCES;

	case GMM_CAUSE_MSC_TEMP_NOTREACH ... GMM_CAUSE_CONGESTION:
		return EHOSTUNREACH;

	case GMM_CAUSE_SEM_INCORR_MSG ... GMM_CAUSE_PROTO_ERR_UNSPEC:
	default:
		return EINVAL;
	}
}

static int subscr_handle_sup_upd_loc_err(struct gsm_subscriber *subscr,
									struct gprs_gsup_message *gsup_msg)
{
	int cause_err;
	struct gsm_subscriber_connection *conn = connection_for_subscr(subscr);

	cause_err = check_cause(gsup_msg->cause);

	LOGGSUBSCRP(LOGL_DEBUG, subscr,
		"Update location has failed with cause %d, handled as: %s\n",
		gsup_msg->cause, strerror(cause_err));

	switch (cause_err) {
	case EACCES:
		LOGGSUBSCRP(LOGL_NOTICE, subscr,
			"GSM update location failed, access denied, "
			"MM cause = '%s' (%d)\n",
			get_value_string(gsm48_gmm_cause_names, gsup_msg->cause),
			gsup_msg->cause);
		gsm0408_loc_upd_rej(conn, gsup_msg->cause);
		release_loc_updating_req(conn, 0);
		break;

	case EHOSTUNREACH:
		LOGGSUBSCRP(LOGL_NOTICE, subscr,
			"GSM update location failed, MM cause = '%s' (%d)\n",
			get_value_string(gsm48_gmm_cause_names, gsup_msg->cause),
			gsup_msg->cause);
		// TODO: Try to find subscriber in local HLR?
		gsm0408_loc_upd_rej(conn, gsup_msg->cause);
		release_loc_updating_req(conn, 0);
		break;

	default:
	case EINVAL:
		LOGGSUBSCRP(LOGL_ERROR, subscr,
			"SUP protocol remote error, MM cause = '%s' (%d)\n",
			get_value_string(gsm48_gmm_cause_names, gsup_msg->cause),
			gsup_msg->cause);
		break;
	}

	return -gsup_msg->cause;
}

static int subscr_handle_sup_auth_err(struct gsm_subscriber *subscr,
					    struct gprs_gsup_message *gsup_msg)
{
	int cause_err;
	struct gsm_subscriber_connection *conn = connection_for_subscr(subscr);
	gsm_cbfn *cb = conn->sec_operation->cb;

	cause_err = check_cause(gsup_msg->cause);

	LOGGSUBSCRP(LOGL_DEBUG, subscr,
		"Send authentication info has failed with cause %d, "
		"handled as: %s\n",
		gsup_msg->cause, strerror(cause_err));

	switch (cause_err) {
	case EACCES:
		LOGGSUBSCRP(LOGL_NOTICE, subscr,
			"GSM send auth info req failed, access denied, "
			"MM cause = '%s' (%d)\n",
			get_value_string(gsm48_gmm_cause_names, gsup_msg->cause),
			gsup_msg->cause);
		if (cb)
			cb(GSM_HOOK_RR_SECURITY, GSM_SECURITY_AUTH_FAILED,
			   NULL, conn, conn->sec_operation->cb_data);
		release_security_operation(conn);
		break;

	case EHOSTUNREACH:
		LOGGSUBSCRP(LOGL_NOTICE, subscr,
			"GSM send auth info req failed, MM cause = '%s' (%d)\n",
			get_value_string(gsm48_gmm_cause_names, gsup_msg->cause),
			gsup_msg->cause);
		// TODO: Try to resend auth request?
		if (cb)
			cb(GSM_HOOK_RR_SECURITY, GSM_SECURITY_AUTH_FAILED,
			   NULL, conn, conn->sec_operation->cb_data);
		release_security_operation(conn);
		break;

	default:
	case EINVAL:
		LOGGSUBSCRP(LOGL_ERROR, subscr,
			"SUP protocol remote error, MM cause = '%s' (%d)\n",
			get_value_string(gsm48_gmm_cause_names, gsup_msg->cause),
			gsup_msg->cause);
		break;
	}

	return -gsup_msg->cause;
}

static int subscr_handle_unknown_imsi(struct gprs_gsup_client *sup_client,
									  struct gprs_gsup_message *gsup_msg)
{
	if (GPRS_GSUP_IS_MSGT_REQUEST(gsup_msg->message_type)) {
		subscr_tx_sup_error_reply(sup_client, NULL, gsup_msg,
						GMM_CAUSE_IMSI_UNKNOWN);
		LOGP(DSUP, LOGL_NOTICE,
		     "Unknown IMSI %s, discarding SUP request "
		     "of type 0x%02x\n",
		     gsup_msg->imsi, gsup_msg->message_type);
	} else if (GPRS_GSUP_IS_MSGT_ERROR(gsup_msg->message_type)) {
		LOGP(DSUP, LOGL_NOTICE,
		     "Unknown IMSI %s, discarding SUP error "
		     "of type 0x%02x, cause '%s' (%d)\n",
		     gsup_msg->imsi, gsup_msg->message_type,
		     get_value_string(gsm48_gmm_cause_names, gsup_msg->cause),
		     gsup_msg->cause);
	} else {
		LOGP(DSUP, LOGL_NOTICE,
		     "Unknown IMSI %s, discarding SUP response "
		     "of type 0x%02x\n",
		     gsup_msg->imsi, gsup_msg->message_type);
	}

	return -GMM_CAUSE_IMSI_UNKNOWN;
}

static int subscr_rx_sup_message(struct gprs_gsup_client *sup_client, struct msgb *msg)
{
	uint8_t *data = msgb_l2(msg);
	size_t data_len = msgb_l2len(msg);
	int rc = 0;

	struct gprs_gsup_message gsup_msg = {0};
	struct gsm_subscriber *subscr;

    if (*data == GPRS_GSUP_MSGT_MAP) {
        return rx_uss_message(data, data_len);
    }

	rc = gprs_gsup_decode(data, data_len, &gsup_msg);
	if (rc < 0) {
		LOGP(DSUP, LOGL_ERROR,
		     "decoding SUP message fails with error '%s' (%d)\n",
		     get_value_string(gsm48_gmm_cause_names, -rc), -rc);
		return rc;
	}

	if (!gsup_msg.imsi[0]) {
		LOGP(DSUP, LOGL_ERROR, "Missing IMSI in SUP message\n");

		if (GPRS_GSUP_IS_MSGT_REQUEST(gsup_msg.message_type))
			subscr_tx_sup_error_reply(sup_client, NULL, &gsup_msg,
							GMM_CAUSE_INV_MAND_INFO);
		return -GMM_CAUSE_INV_MAND_INFO;
	}

	if (!gsup_msg.cause && GPRS_GSUP_IS_MSGT_ERROR(gsup_msg.message_type))
		gsup_msg.cause = GMM_CAUSE_NET_FAIL;

	subscr = subscr_get_by_imsi(NULL, gsup_msg.imsi);

	if (!subscr) {
		return subscr_handle_unknown_imsi(sup_client, &gsup_msg);
	}

	LOGGSUBSCRP(LOGL_INFO, subscr,
		"Received SUP message of type 0x%02x\n", gsup_msg.message_type);

	switch (gsup_msg.message_type) {

	case GPRS_GSUP_MSGT_SEND_AUTH_INFO_RESULT:
		rc = subscr_handle_sup_auth_res(sup_client, subscr, &gsup_msg);
		break;

	case GPRS_GSUP_MSGT_SEND_AUTH_INFO_ERROR:
		rc = subscr_handle_sup_auth_err(subscr, &gsup_msg);
		break;

	case GPRS_GSUP_MSGT_UPDATE_LOCATION_RESULT:
		rc = subscr_handle_sup_upd_loc_res(subscr, &gsup_msg);
		break;

	case GPRS_GSUP_MSGT_UPDATE_LOCATION_ERROR:
		rc = subscr_handle_sup_upd_loc_err(subscr, &gsup_msg);
		break;

	case GPRS_GSUP_MSGT_LOCATION_CANCEL_REQUEST:
	case GPRS_GSUP_MSGT_PURGE_MS_ERROR:
	case GPRS_GSUP_MSGT_PURGE_MS_RESULT:
	case GPRS_GSUP_MSGT_INSERT_DATA_REQUEST:
	case GPRS_GSUP_MSGT_DELETE_DATA_REQUEST:
		LOGGSUBSCRP(LOGL_ERROR, subscr,
			"Rx SUP message type %d not yet implemented\n",
			gsup_msg.message_type);
		subscr_tx_sup_error_reply(sup_client, subscr, &gsup_msg,
						GMM_CAUSE_MSGT_NOTEXIST_NOTIMPL);
		rc = -GMM_CAUSE_MSGT_NOTEXIST_NOTIMPL;
		break;

	default:
		LOGGSUBSCRP(LOGL_ERROR, subscr,
			"Rx SUP message type %d not valid at SGSN\n",
			gsup_msg.message_type);
		if (GPRS_GSUP_IS_MSGT_REQUEST(gsup_msg.message_type))
			subscr_tx_sup_error_reply(sup_client, subscr, &gsup_msg,
							GMM_CAUSE_MSGT_NOTEXIST_NOTIMPL);
		rc = -GMM_CAUSE_MSGT_NOTEXIST_NOTIMPL;
		break;
	};

	subscr_put(subscr);

	return rc;
}

int sup_read_cb(struct gprs_gsup_client *sup_client, struct msgb *msg)
{
	int rc;

	rc = subscr_rx_sup_message(sup_client, msg);
	msgb_free(msg);
	if (rc < 0)
		return -1;

	return rc;
}
