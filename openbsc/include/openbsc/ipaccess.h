#ifndef _IPACCESS_H
#define _IPACCESS_H

#include "e1_input.h"

struct ipaccess_head {
	u_int16_t len;	/* network byte order */
	u_int8_t proto;
	u_int8_t data[0];
} __attribute__ ((packed));

enum ipaccess_proto {
	IPAC_PROTO_RSL		= 0x00,
	IPAC_PROTO_IPACCESS	= 0xfe,
	IPAC_PROTO_SCCP		= 0xfd,
	IPAC_PROTO_OML		= 0xff,
};

enum ipaccess_msgtype {
	IPAC_MSGT_PING		= 0x00,
	IPAC_MSGT_PONG		= 0x01,
	IPAC_MSGT_ID_GET	= 0x04,
	IPAC_MSGT_ID_RESP	= 0x05,
	IPAC_MSGT_ID_ACK	= 0x06,
};

enum ipaccess_id_tags {
	IPAC_IDTAG_SERNR		= 0x00,
	IPAC_IDTAG_UNITNAME		= 0x01,
	IPAC_IDTAG_LOCATION1		= 0x02,
	IPAC_IDTAG_LOCATION2		= 0x03,
	IPAC_IDTAG_EQUIPVERS		= 0x04,
	IPAC_IDTAG_SWVERSION		= 0x05,
	IPAC_IDTAG_IPADDR		= 0x06,
	IPAC_IDTAG_MACADDR		= 0x07,
	IPAC_IDTAG_UNIT			= 0x08,
};

int ipaccess_connect(struct e1inp_line *line, struct sockaddr_in *sa);

/*
 * methods for parsing and sending a message
 */
int ipaccess_rcvmsg_base(struct msgb *msg, struct bsc_fd *bfd);
struct msgb *ipaccess_read_msg(struct bsc_fd *bfd, int *error);
void ipaccess_prepend_header(struct msgb *msg, int proto);
int ipaccess_send_id_ack(int fd);
int ipaccess_send_id_req(int fd);

int ipaccess_idtag_parse(struct tlv_parsed *dec, unsigned char *buf, int len);

#endif /* _IPACCESS_H */
