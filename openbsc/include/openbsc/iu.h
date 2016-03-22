#pragma once

struct sgsn_pdp_ctx;
struct msgb;
struct gprs_ra_id;

struct RANAP_RAB_SetupOrModifiedItemIEs_s;
struct RANAP_GlobalRNC_ID;

struct ue_conn_ctx {
	struct llist_head list;
	struct osmo_sua_link *link;
	uint32_t conn_id;
	int integrity_active;
};

enum iu_event_type {
	IU_EVENT_RAB_ASSIGN,
	IU_EVENT_SECURITY_MODE_COMPLETE,
	IU_EVENT_IU_RELEASE, /* An actual Iu Release message was received */
	IU_EVENT_LINK_INVALIDATED, /* A SUA link was lost or closed down */
	/* FIXME: maybe IU_EVENT_IU_RELEASE and IU_EVENT_LINK_INVALIDATED
	 * should be combined to one generic event that simply means the
	 * ue_conn_ctx should no longer be used, for whatever reason. */
};

/* Implementations of iu_recv_cb_t shall find the ue_conn_ctx in msg->dst. */
typedef int (* iu_recv_cb_t )(struct msgb *msg, struct gprs_ra_id *ra_id,
			      /* TODO "gprs_" in generic CS+PS domain ^ */
			      uint16_t *sai);

typedef int (* iu_event_cb_t )(struct ue_conn_ctx *ue_ctx,
			       enum iu_event_type type, void *data);

typedef int (* iu_rab_ass_resp_cb_t )(struct ue_conn_ctx *ue_ctx, uint8_t rab_id,
		struct RANAP_RAB_SetupOrModifiedItemIEs_s *setup_ies);

int iu_init(void *ctx, const char *listen_addr, uint16_t listen_port,
	    iu_recv_cb_t iu_recv_cb, iu_event_cb_t iu_event_cb);

void iu_link_del(struct osmo_sua_link *link);

int iu_tx(struct msgb *msg, uint8_t sapi);

int iu_rab_act_cs(struct ue_conn_ctx *ue_ctx, uint32_t rtp_ip, uint16_t rtp_port);
int iu_rab_act_ps(uint8_t rab_id, struct sgsn_pdp_ctx *pdp);
int iu_rab_deact(struct ue_conn_ctx *ue_ctx, uint8_t rab_id);
int iu_tx_sec_mode_cmd(struct ue_conn_ctx *uectx, struct gsm_auth_tuple *tp,
		       int send_ck, int new_key);
