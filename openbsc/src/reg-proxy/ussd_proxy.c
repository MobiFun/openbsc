#ifdef HAVE_CONFIG_H
//#include <config.h>
#endif

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

typedef struct context_s context_t;
#define NTA_OUTGOING_MAGIC_T context_t
#define SU_ROOT_MAGIC_T      context_t
#define NUA_MAGIC_T          context_t

typedef struct operation operation_t;
#define NUA_HMAGIC_T         operation_t

#include <sofia-sip/nta.h>
#include <sofia-sip/nua.h>
#include <sofia-sip/sip_header.h>
#include <sofia-sip/sip_tag.h>
#include <sofia-sip/su_tag_io.h>
#include <sofia-sip/sl_utils.h>
#include <sofia-sip/sip_util.h>
#include <sofia-sip/auth_client.h>
#include <sofia-sip/tport_tag.h>

#include <osmocom/abis/ipa.h>
#include <osmocom/core/application.h>
#include <osmocom/core/logging.h>
#include <osmocom/gsm/ipa.h>
#include <osmocom/gsm/protocol/ipaccess.h>
#include <osmocom/gsm/gsm0480.h>

#include <openbsc/gprs_gsup_messages.h>


/******************************************************************************/
/* Put this into separate file */

enum {
    FMAP_MSISDN        = 0x80
};
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
	const_data += 2;

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
			DEBUGP(DLCTRL, "Unknown code: %d\n", *const_data);
			return -1;
		}
	}

	return 0;
}

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

	/* invokeId */
	msgb_tlv_put(msg, GSM0480_COMPIDTAG_INVOKE_ID, 1, &req->invoke_id);

	/* opCode */
	msgb_tlv_put(msg, GSM0480_OPERATION_CODE, 1, &req->opcode);

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
	gsup_indicator[3] = 3 + 3 + (req->ussd_text_len + 2) + (bcd_len + 2);

	/* wrap with GSM0480_CTYPE_INVOKE */
	// gsm0480_wrap_invoke(msg, req->opcode, invoke_id);
	// gsup_indicator = msgb_push(msgb, 1);
	// gsup_indicator[0] = GPRS_GSUP_MSGT_MAP;
	return 0;
}

/******************************************************************************/


typedef struct isup_connection isup_connection_t;

struct isup_connection {
	context_t      *ctx;

	su_socket_t     isup_conn_socket;
	su_wait_t       isup_conn_event;
	int             isup_register_idx;

	/* osmocom data */

	struct msgb    *pending_msg;
};

struct ussd_session {
	isup_connection_t  *conn;

	// int32_t             transaction_id;

	int                 ms_originated;
	struct ss_request   rigester_msg;

	char                extention[32];
};

struct context_s {
	su_home_t       home[1];
	su_root_t      *root;

	su_socket_t     isup_acc_socket;
	su_wait_t       isup_acc_event;

	nua_t          *nua;

	const char     *to_str;

	/* Array of isup connections */
	struct isup_connection isup[1];

	/* list of active operations */
	struct operation* operations;
};


/* Example of operation handle context information structure */
struct operation
{
	nua_handle_t    *handle;  /* operation handle */
	context_t       *ctx;

	/* protocol specific sessions */
	struct ussd_session ussd;
};


static
int response_to_options(sip_t const *sip);

static
int ussd_send_data(operation_t *op, int last, const char* lang, unsigned lang_len,
		   const char* msg, unsigned msg_len);


static const char* get_unknown_header(sip_t const *sip, const char *header)
{
	sip_header_t *h = (sip_header_t *)sip->sip_unknown;
	for (; h; h = (sip_header_t *)h->sh_succ) {
		if (strcasecmp(h->sh_unknown->un_name, header) == 0) {
			return h->sh_unknown->un_value;
		}
	}
	return NULL;
}


int sup_server_send(operation_t *op, struct msgb *msg)
{
	if (!op->ussd.conn) {
		msgb_free(msg);
		return -ENOTCONN;
	}

	ipa_prepend_header_ext(msg, IPAC_PROTO_EXT_GSUP);
	ipa_msg_push_header(msg, IPAC_PROTO_OSMO);

	LOGP(DLCTRL, LOGL_ERROR,
	     "Sending wire, will send: %s\n", msgb_hexdump(msg));

	// FIXME ugly hack!!!
	// TODO place message in send queue !!!!
	send(op->ussd.conn->isup_conn_socket, msg->data, msg->len, 0);
	msgb_free(msg);

	return 0;
}

static int ussd_parse_xml(const char *xml,
			  unsigned xml_len,
			  const char **lang,
			  unsigned    *lang_len,
			  const char **msg,
			  unsigned    *msg_len)
{
	/* Example of parsing XML
		<?xml version="1.0" encoding="UTF-8"?>
		<ussd-data>
			<language>en</language>
			<ussd-string>Test</ussd-string>
		</ussd-data>
	*/

	// <ussd-data> tag
	char* ussd_data_stag = strstr(xml, "<ussd-data>");
	if (ussd_data_stag == NULL)
		return 0;

	char* ussd_data_etag = strstr(ussd_data_stag, "</ussd-data>");
	if (ussd_data_etag == NULL)
		return 0;

	// <language> tag
	char* ussd_lang_stag = strstr(ussd_data_stag, "<language>");
	if (ussd_lang_stag == NULL)
		return 0;

	char* ussd_lang_etag = strstr(ussd_lang_stag, "</language>");
	if (ussd_lang_etag == NULL)
		return 0;

	// <language> tag
	char* ussd_ussd_stag = strstr(ussd_data_stag, "<ussd-string>");
	if (ussd_ussd_stag == NULL)
		return 0;

	char* ussd_ussd_etag = strstr(ussd_ussd_stag, "</ussd-string>");
	if (ussd_ussd_etag == NULL)
		return 0;

	if (ussd_ussd_etag - xml > xml_len || ussd_lang_etag - xml > xml_len)
		return 0;

	*lang = ussd_lang_stag + strlen("<language>");
	*lang_len = ussd_lang_etag - *lang;

	*msg = ussd_ussd_stag + strlen("<ussd-string>");
	*msg_len = ussd_ussd_etag - *msg;

	return 1;
}

void proxy_r_invite(int           status,
		    char const   *phrase,
		    nua_t        *nua,
		    nua_magic_t  *magic,
		    nua_handle_t *nh,
		    nua_hmagic_t *hmagic,
		    sip_t const  *sip,
		    tagi_t        tags[])
{
	if (status == 200) {
		nua_ack(nh, TAG_END());
	} else {
		printf("response to INVITE: %03d %s\n", status, phrase);
	}
}

void proxy_r_bye(int           status,
		 char const   *phrase,
		 nua_t        *nua,
		 nua_magic_t  *magic,
		 nua_handle_t *nh,
		 nua_hmagic_t *hmagic,
		 sip_t const  *sip,
		 tagi_t        tags[])
{
	const char* ri;
	int rc;
	// printf("*** call released:\n%s\n", sip->sip_payload->pl_data);

	ri = get_unknown_header(sip, "Recv-Info");
	if (ri && (strcasecmp(ri, "g.3gpp.ussd") == 0)) {
		/* Parse XML */
		const char *language;
		const char *msg;
		unsigned language_len;
		unsigned msg_len;

		if (ussd_parse_xml(sip->sip_payload->pl_data,
				   sip->sip_payload->pl_len,
				   &language, &language_len,
				   &msg, &msg_len)) {
			printf("=== USSD (%.*s): %.*s\n",
			       language_len, language,
			       msg_len, msg);

			/* Send reply back to SUP */
			// TODO !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
			rc = ussd_send_data(hmagic, 1, language, language_len,
					    msg, msg_len);

			// TODO handle err, if socket is unavailable we MUST
			// terminate sip session
		} else {
			fprintf(stderr, "*** unable to parse XML\n");
		}
	}

	/* release operation handle */
	nua_handle_destroy(hmagic->handle);
	hmagic->handle = NULL;

	/* release operation context information */
	su_free(hmagic->ctx->home, hmagic);

}

int ussd_create_xml_ascii(char *content, size_t max_len, const char* language, const char* msg, int msg_len)
{
	int content_len = snprintf(content, max_len,
				   "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
				   "<ussd-data>\n"
				   "<language>%s</language>\n"
				   "<ussd-string>%.*s</ussd-string>\n"
				   "</ussd-data>",
				   language,
				   msg_len, msg);
	if (content_len > max_len) {
		content[max_len - 1] = 0;
		return 0;
	}
	return 1;
}

operation_t *open_ussd_session_mo(context_t* ctx,
				  isup_connection_t *conn,
				  struct ss_request* ss,
				  const char* extention)
{
	char content[1024];
	operation_t *op;
	sip_to_t *to;

	/* create operation context information */
	op = su_zalloc(ctx->home, (sizeof *op));
	if (!op) {
		goto failed_alloc;
	}

	/* Destination address */
	to = sip_to_create(ctx->home, (url_string_t *)ctx->to_str);
	if (!to) {
		goto failed_create;
	}
	//to->a_display = name;

	/* create operation handle */
	op->handle = nua_handle(ctx->nua,
				op,
				SIPTAG_TO(to),
				TAG_END());
	if (op->handle == NULL) {
		goto failed_sip_addr;
	}

	op->ctx = ctx;
	op->ussd.conn = conn;
	op->ussd.ms_originated = 1;
	op->ussd.rigester_msg = *ss;
	strncpy(op->ussd.extention, extention, sizeof(op->ussd.extention));

	/* TODO add language support !!! */

	if (!ussd_create_xml_ascii(content, sizeof(content), "en",
				   (const char* )op->ussd.rigester_msg.ussd_text,
				   op->ussd.rigester_msg.ussd_text_len)) {
		goto failed_nua;
	}

	nua_invite(op->handle,
		   /* other tags as needed ... */
		   SIPTAG_CONTENT_TYPE_STR("application/vnd.3gpp.ussd+xml"),
		   SIPTAG_PAYLOAD_STR(content),
		   TAG_END());

	return op;

failed_nua:
	nua_handle_destroy(op->handle);
failed_sip_addr:
	su_free(ctx->home, to);
failed_create:
	su_free(ctx->home, op);
failed_alloc:
	fprintf(stderr, "*** open_ussd_session failed!\n");
	return NULL;
}



void context_callback(nua_event_t   event,
		      int           status,
		      char const   *phrase,
		      nua_t        *nua,
		      nua_magic_t  *magic,
		      nua_handle_t *nh,
		      nua_hmagic_t *hmagic,
		      sip_t const  *sip,
		      tagi_t        tags[])
{
	switch (event) {
	case nua_i_bye:
		proxy_r_bye(status, phrase, nua, magic, nh, hmagic, sip, tags);
		break;

	case nua_i_invite:
		//app_i_invite(status, phrase, nua, magic, nh, hmagic, sip, tags);
		break;

	case nua_r_invite:
		proxy_r_invite(status, phrase, nua, magic, nh, hmagic, sip, tags);
		break;

		/* and so on ... */

	default:
		/* unknown event -> print out error message */
		if (status > 100) {
			printf("unknown event %d: %03d %s\n",
			       event,
			       status,
			       phrase);
		} else {
			printf("unknown event %d\n", event);
		}
		tl_print(stdout, "", tags);
		break;
	}
}

static int rx_sup_uss_message(isup_connection_t *sup_conn, const uint8_t* data, size_t len)
{
	char extention[32] = {0};
	struct ss_request ss;
	context_t *ctx = sup_conn->ctx;
	memset(&ss, 0, sizeof(ss));

	if (rx_uss_message_parse(&ss, data, len, extention, sizeof(extention))) {
		LOGP(DLCTRL, LOGL_ERROR, "Can't parse uss message\n");
		return -1;
	}

	LOGP(DLCTRL, LOGL_ERROR, "Got mtype=0x%02x invoke_id=0x%02x opcode=0x%02x component_type=0x%02x text=%s\n",
	     ss.message_type, ss.invoke_id, ss.opcode, ss.component_type, ss.ussd_text);

	switch (ss.message_type) {
	case GSM0480_MTYPE_REGISTER:
		/* Create new session */
	{
		if (ctx->operations) {
			LOGP(DLCTRL, LOGL_ERROR, "Doesn't support multiple sessions. Failing this for now\n");
			return -1;
		}

		operation_t * newop = open_ussd_session_mo(ctx,
							   sup_conn,
							   &ss,
							   extention);

		ctx->operations = newop;

	}
		break;

	case GSM0480_MTYPE_FACILITY:
		break;

	case GSM0480_MTYPE_RELEASE_COMPLETE:
		break;
	}

	return 0;
}

int ussd_send_data(operation_t *op, int last, const char* lang, unsigned lang_len,
		   const char* msg, unsigned msg_len)
{
	struct ss_request ss;
	memset(&ss, 0, sizeof(ss));

	// TODO handle language

	if (last) {
		ss.message_type = GSM0480_MTYPE_RELEASE_COMPLETE;
		ss.component_type = GSM0480_CTYPE_RETURN_RESULT;
	} else {
		ss.message_type = GSM0480_MTYPE_FACILITY;
		ss.component_type = (op->ussd.rigester_msg.component_type == GSM0480_CTYPE_INVOKE) ?
					GSM0480_CTYPE_RETURN_RESULT : GSM0480_CTYPE_INVOKE;
	}

	ss.opcode = op->ussd.rigester_msg.opcode;
	ss.invoke_id = op->ussd.rigester_msg.invoke_id;

	if (msg_len > MAX_LEN_USSD_STRING) {
		msg_len = MAX_LEN_USSD_STRING;
	}

	ss.ussd_text_len = msg_len;
	strncpy(ss.ussd_text, msg, msg_len);

	struct msgb *outmsg = msgb_alloc_headroom(4000, 64, __func__);
	subscr_uss_message(outmsg,
			   &ss,
			   NULL);

	LOGP(DLCTRL, LOGL_ERROR,
	     "Sending USS, will send: %s\n", msgb_hexdump(outmsg));

	return sup_server_send(op, outmsg);
}

static int isup_handle_connection(context_t *cli, su_wait_t *w, void *p)
{
	int rc;
	isup_connection_t *conn = (isup_connection_t*)p;

	int events = su_wait_events(w, conn->isup_conn_socket);
	printf("*** connection; event=0x%x\n", events);

	if (events & (SU_WAIT_ERR | SU_WAIT_HUP)) {
		printf("*** connection destroyed\n");
		goto err;
	} else if (events & SU_WAIT_IN) {
		/* Incoming data */

		struct ipaccess_head *iph;
		struct msgb *msg = NULL;
		int ret = ipa_msg_recv_buffered(conn->isup_conn_socket, &msg, &conn->pending_msg);
		if (ret <= 0) {
			if (ret == -EAGAIN)
				return 0;
			if (ret == 0)
				LOGP(DLCTRL, LOGL_INFO, "The control connection was closed\n");
			else
				LOGP(DLCTRL, LOGL_ERROR, "Failed to parse ip access message: %d\n", ret);

			goto err;
		}

		iph = (struct ipaccess_head *) msg->data;
		switch (iph->proto)
		{
		case IPAC_PROTO_IPACCESS:
			if (msg->l2h[0] == IPAC_MSGT_PING) {
				printf("*** got PING\n");
				msg->l2h[0] = IPAC_MSGT_PONG;
				send(conn->isup_conn_socket, msg->data, ntohs(iph->len) + sizeof(struct ipaccess_head), 0);
				msgb_free(msg);
				conn->pending_msg = NULL;
				return 0;
			}

			LOGP(DLCTRL, LOGL_ERROR, "Unknown IPAC_PROTO_IPACCESS msg 0x%x\n", msg->l2h[0]);
			goto err;
		case IPAC_PROTO_OSMO:
			// TODO callback
			if (msg->l2h[1] == GPRS_GSUP_MSGT_MAP) {
				LOGP(DLCTRL, LOGL_ERROR,
					   "Receive USS: %s\n", msgb_hexdump(msg));

				rc = rx_sup_uss_message(conn, &msg->l2h[1], msgb_l2len(msg) - 1);
				if (rc < 0) {
					/* TODO raise reject !!!!!!! */
					/* release complete */
				}

				msgb_free(msg);
				conn->pending_msg = NULL;
				return 0;
			}

			/* TODO: handle gprs_gsup_decode() for other types */

			LOGP(DLCTRL, LOGL_ERROR, "Unknown IPAC_PROTO_OSMO GPRS_GSUP_MSGT_* 0x%x\n", msg->l2h[1]);
			msgb_free(msg);
			conn->pending_msg = NULL;
			goto err;
		default:
			LOGP(DLCTRL, LOGL_ERROR, "Protocol mismatch. We got 0x%x\n", iph->proto);
			goto err;
		}
	}

	return 0;

err:
	close(conn->isup_conn_socket);
	conn->isup_conn_socket = INVALID_SOCKET;

	su_wait_destroy(w);

	msgb_free(conn->pending_msg);
	conn->pending_msg = NULL;
	//su_root_deregister(cli, cli->isup_register_idx);
	return 0;
}

static int isup_handle_accept(context_t *cli, su_wait_t *w, void *p)
{
	su_sockaddr_t aaddr;
	su_socket_t connection;
	socklen_t len = sizeof(aaddr);
	int rc;

	connection = accept(cli->isup_acc_socket, &aaddr.su_sa, &len);
	if (connection == INVALID_SOCKET) {
		perror("can't accept isup socket");
		return 0;
	}

	printf("*** accepted from %s:%d\n",
	       inet_ntoa(aaddr.su_sin.sin_addr),
	       ntohs(aaddr.su_sin.sin_port));

	/* TODO manage isup connection list, but just now use the single connection */
	isup_connection_t *conn = cli->isup;
	if (conn->isup_conn_socket != INVALID_SOCKET) {
		fprintf(stderr, "--- Can't accept, there's another connection\n");
		su_close(connection);
		return 0;
	}

	conn->ctx = cli;
	conn->isup_conn_socket = connection;
	conn->pending_msg = NULL;

	su_wait_init(&conn->isup_conn_event);
	rc = su_wait_create(&conn->isup_conn_event,
			    conn->isup_conn_socket,
			    SU_WAIT_IN | /*SU_WAIT_OUT | */ SU_WAIT_HUP | SU_WAIT_ERR);

	conn->isup_register_idx = su_root_register(cli->root,
						   &conn->isup_conn_event,
						   isup_handle_connection,
						   conn,
						   0);
	return 0;
}

#define DIPA_USSD_PROXY 0

struct log_info_cat ipa_proxy_test_cat[] = {
	[DIPA_USSD_PROXY] = {
		.name = "DIPA_USSD_PROXY",
		.description = "USSD_PROXY",
		.color = "\033[1;35m",
		.enabled = 1,
		.loglevel = LOGL_DEBUG,
	},
};

const struct log_info ipa_proxy_test_log_info = {
	.filter_fn = NULL,
	.cat = ipa_proxy_test_cat,
	.num_cat = ARRAY_SIZE(ipa_proxy_test_cat),
};


int main(int argc, char *argv[])
{
	su_home_t *home;
	context_t context[1] = {{{SU_HOME_INIT(context)}}};
	su_sockaddr_t listen_addr;
	int rc;
	int port = 8184;
	const char* to_str = "sip:ussd@127.0.0.1:5060";

	osmo_init_logging(&ipa_proxy_test_log_info);

	su_init();
	su_home_init(home = context->home);

	context->root = su_root_create(context);

	su_log_set_level (NULL, 9);

	/* Disable threading */
	su_root_threading(context->root, 0);

	if (!context->root) {
		fprintf(stderr, "Unable to initialize sip-sofia context\n");
		return 1;
	}

	context->isup_acc_socket = su_socket(AF_INET, SOCK_STREAM, 0);
	if (context->isup_acc_socket == INVALID_SOCKET) {
		perror("unable to create socket\n");
		return 1;
	}
	su_setblocking(context->isup_acc_socket, 0);
	su_setreuseaddr(context->isup_acc_socket, 1);

	context->isup->isup_conn_socket = INVALID_SOCKET;

	memset(&listen_addr, 0, sizeof(listen_addr));
	listen_addr.su_sin.sin_family = AF_INET;
	listen_addr.su_sin.sin_addr.s_addr = INADDR_ANY;
	listen_addr.su_sin.sin_port = htons(port);

	rc = bind(context->isup_acc_socket, &listen_addr.su_sa, sizeof(listen_addr.su_sin));
	if (rc < 0) {
		perror("cannot bind socket\n");
		return 2;
	}

	rc = listen(context->isup_acc_socket, 1);
	if (rc < 0) {
		perror("cannot bind socket\n");
		return 2;
	}

	su_wait_init(&context->isup_acc_event);
	su_wait_create(&context->isup_acc_event, context->isup_acc_socket, SU_WAIT_ACCEPT);
	su_root_register(context->root,
			 &context->isup_acc_event,
			 isup_handle_accept,
			 NULL,
			 0);

	context->to_str = to_str;
	context->nua = nua_create(context->root,
				  context_callback,
				  context,
				  NUTAG_URL ("sip:ussd_proxy@127.0.0.1:5090"),
				  /* tags as necessary ...*/
				  TAG_NULL());
	if (context->nua == NULL) {
		fprintf(stderr, "Unable to initialize sip-sofia nua\n");
		return 1;
	}

	nua_set_params(context->nua,
		       NUTAG_ENABLEINVITE(1),
		       NUTAG_AUTOALERT(1),
		       NUTAG_SESSION_TIMER(0),
		       NUTAG_AUTOANSWER(0),
		       NUTAG_MEDIA_ENABLE(0),
		       //NTATAG_UDP_MTU(100),    //////////////// FORCE TCP
		       TAG_NULL());

	context->operations = NULL;

	// TEST only
	// open_ussd_session(context);

	/* enter main loop for processing of messages */
	su_root_run(context->root);

	nua_destroy(context->nua);

#if 0
	url_string_t *r_uri;

	context->c_agent =
			nta_agent_create(context->c_root,
					 URL_STRING_MAKE(o_bind),
					 NULL,
					 NULL, /* Ignore incoming messages */
					 TPTAG_HTTP_CONNECT(o_http_proxy),
					 TAG_END());

	if (!context->c_agent) {
		fprintf(stderr, "Unable to create sip-sofia agent\n");
		return 1;
	}

	sip_addr_t *from, *to;
	sip_contact_t const *m = nta_agent_contact(context->c_agent);

	to = sip_to_create(home, (url_string_t *)o_to);

	if (o_from)
		from = sip_from_make(home, o_from);
	else
		from = sip_from_create(home, (url_string_t const *)m->m_url);

	if (!from) {
		fprintf(stderr, "%s: no valid From address\n", name);
		exit(2);
	}

	//  tag_from_header(context->c_agent, context->c_home, from);

	if (o_method) {
		method = sip_method_code(o_method);
	} else {
		isize_t len;
		char const *params = to->a_url->url_params;

		len = url_param(params, "method", NULL, 0);

		if (len > 0) {
			o_method = su_alloc(home, len + 1);
			if (o_method == 0 ||
					url_param(params, "method", o_method, len + 1) != len) {
				fprintf(stderr, "%s: %s\n", name,
					o_method ? "internal error" : strerror(errno));
				exit(2);
			}
			method = sip_method_code(o_method);
		}
	}

	r_uri = (url_string_t *)url_hdup(home, to->a_url);

	sip_aor_strip(to->a_url);
	sip_aor_strip(from->a_url);

	context->c_username = from->a_url->url_user;
	context->c_password = from->a_url->url_password;
	from->a_url->url_password = NULL;

	if (extra) {
		FILE *hf;

		if (strcmp(extra, "-"))
			hf = fopen(extra, "rb");
		else
			hf = stdin;

		extra = readfile(hf);
	}

	context->c_proxy = url_hdup(context->c_home,
				    (url_t *)getenv("sip_proxy"));

	nta_agent_set_params(context->c_agent,
			     NTATAG_SIPFLAGS(MSG_FLG_EXTRACT_COPY),
			     NTATAG_DEFAULT_PROXY(context->c_proxy),
			     TAG_END());

	context->c_leg =
			nta_leg_tcreate(context->c_agent,
					NULL, NULL,      /* ignore incoming requests */
					SIPTAG_FROM(from), /* who is sending OPTIONS? */
					SIPTAG_TO(to), /* whom we are sending OPTIONS? */
					TAG_END());

	if (context->c_leg) {
		context->c_orq =
				nta_outgoing_tcreate(context->c_leg,
						     response_to_options, context,
						     NULL,
						     method, o_method, r_uri,
						     SIPTAG_USER_AGENT_STR("options"),
						     SIPTAG_MAX_FORWARDS_STR(o_max_forwards),
						     SIPTAG_HEADER_STR(extra),
						     TAG_END());

		if (context->c_orq) {
			su_root_run(context->c_root);
			nta_outgoing_destroy(context->c_orq); context->c_orq = NULL;
		}

		nta_leg_destroy(context->c_leg); context->c_leg = NULL;
	}

	nta_agent_destroy(context->c_agent); context->c_agent = NULL;
	su_root_destroy(context->c_root);

	su_deinit();

	return context->c_retval;
#endif
	return 0;
}



/** Handle responses to registration request */
static
int response_to_options(sip_t const *sip)
{

//  if (sip->sip_status->st_status >= 200 || context->c_pre) {
    sip_header_t *h = (sip_header_t *)sip->sip_unknown;
    char hname[64];

    for (; h; h = (sip_header_t *)h->sh_succ) {
      /*if (!context->c_all) {
	if (sip_is_from(h) ||
	    sip_is_via(h) ||
	    sip_is_call_id(h) ||
	    sip_is_cseq(h) ||
	    sip_is_content_length(h))
	  continue;
      }*/
      if (h->sh_class->hc_name) {
	snprintf(hname, sizeof hname, "%s: %%s\n", h->sh_class->hc_name);
	sl_header_print(stdout, hname, h);
      }
      else {
	sl_header_print(stdout, NULL, h);
      }
    }
//  }

  return 0;
}

