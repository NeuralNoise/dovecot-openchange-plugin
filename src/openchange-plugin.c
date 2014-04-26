/*
   OpenChange notification Plugin for Dovecot

   Copyright (C) Zentyal S.L. 2014

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "openchange-plugin.h"
#include <stdio.h>
#include <dovecot/lib.h>
#include <dovecot/compat.h>
#include <dovecot/llist.h>
#include <dovecot/mail-user.h>
#include <dovecot/mail-storage-hooks.h>
#include <dovecot/mail-storage.h>
#include <dovecot/mail-storage-private.h>
#include <dovecot/module-context.h>
#include <dovecot/notify-plugin.h>
#include <amqp.h>
#include <amqp_tcp_socket.h>
#include <json-c/json.h>

#define	OPENCHANGE_USER_CONTEXT(obj) \
	MODULE_CONTEXT(obj, openchange_user_module)

const char *openchange_plugin_version = DOVECOT_ABI_VERSION;
const char *openchange_plugin_dependencies[] = { "notify", NULL };
struct notify_context *openchange_ctx;

enum openchange_event {
	OPENCHANGE_EVENT_DELETE		= 0x01,
	OPENCHANGE_EVENT_UNDELETE	= 0x02,
	OPENCHANGE_EVENT_EXPUNGE	= 0x04,
	OPENCHANGE_EVENT_SAVE		= 0x08,
	OPENCHANGE_EVENT_COPY		= 0x10,
	OPENCHANGE_EVENT_MAILBOX_CREATE	= 0x20,
	OPENCHANGE_EVENT_MAILBOX_DELETE	= 0x40,
	OPENCHANGE_EVENT_MAILBOX_RENAME	= 0x80,
	OPENCHANGE_EVENT_FLAG_CHANGE	= 0x100,
};

#define	OPENCHANGE_DEFAULT_EVENTS	(OPENCHANGE_EVENT_SAVE)

enum openchange_field {
	OPENCHANGE_FIELD_UID		= 0x1,
	OPENCHANGE_FIELD_BOX		= 0x2,
	OPENCHANGE_FIELD_MSGID		= 0x4,
	OPENCHANGE_FIELD_PSIZE		= 0x8,
	OPENCHANGE_FIELD_VSIZE		= 0x10,
	OPENCHANGE_FIELD_FLAGS		= 0x20,
	OPENCHANGE_FIELD_FROM		= 0x40,
	OPENCHANGE_FIELD_SUBJECT	= 0x80,
};

#define	OPENCHANGE_DEFAULT_FIELDS \
	(OPENCHANGE_FIELD_UID | OPENCHANGE_FIELD_BOX | \
	 OPENCHANGE_FIELD_MSGID | OPENCHANGE_FIELD_PSIZE)

struct openchange_user {
	union mail_user_module_context	module_ctx;
	enum openchange_field		fields;
	enum openchange_event		events;
	const char			*username;
	const char			*broker_host;
	unsigned int			broker_port;
	const char			*broker_user;
	const char			*broker_pass;
	const char			*broker_exchange;
	const char			*broker_routing;
	amqp_connection_state_t		broker_conn;
	amqp_socket_t			*broker_socket;
};

struct openchange_message {
	struct openchange_message	*prev;
	struct openchange_message	*next;
	enum openchange_event		event;
	uint32_t			uid;
	const char			*destination_folder;
};

struct openchange_mail_txn_context {
	pool_t				pool;
	struct mail_namespace		*ns;
	struct openchange_message	*messages;
	struct openchange_message	*messages_tail;
};

static MODULE_CONTEXT_DEFINE_INIT(openchange_user_module,
				  &mail_user_module_register);

	int
openchange_broker_err(char *buffer, int buffer_len, amqp_rpc_reply_t r)
{
	int ret;

	ret = 0;
	switch (r.reply_type) {
	case AMQP_RESPONSE_NORMAL:
		ret = snprintf(buffer, buffer_len, "normal response");
		break;
	case AMQP_RESPONSE_NONE:
		snprintf(buffer, buffer_len, "missing RPC reply type");
		break;
	case AMQP_RESPONSE_LIBRARY_EXCEPTION:
		ret = snprintf(buffer, buffer_len, "%s",
			amqp_error_string2(r.library_error));
		break;
	case AMQP_RESPONSE_SERVER_EXCEPTION:
		switch (r.reply.id) {
		case AMQP_CONNECTION_CLOSE_METHOD:
		{
			amqp_connection_close_t *m;
			m = (amqp_connection_close_t *) r.reply.decoded;
			ret = snprintf(buffer, buffer_len,
				"server connection error %d, message: %.*s",
				m->reply_code,
				(int) m->reply_text.len,
				(char *) m->reply_text.bytes);
			break;
		}
		case AMQP_CHANNEL_CLOSE_METHOD:
		{
			amqp_channel_close_t *m;
			m = (amqp_channel_close_t *) r.reply.decoded;
			ret = snprintf(buffer, buffer_len,
				"server channel error %d, message: %.*s",
				m->reply_code,
				(int) m->reply_text.len,
				(char *) m->reply_text.bytes);
			break;
		}
		default:
		{
			ret = snprintf(buffer, buffer_len,
				"unknown server error, method id 0x%08X",
				r.reply.id);
		}
		break;
		}
		break;
	}

	return ret;
}

/**
 * Close broker connection
 */
	static void
openchange_broker_disconnect(struct openchange_user *user)
{
	amqp_rpc_reply_t r;
	int ret;

	if (user->broker_conn != NULL) {
		i_debug("openchange: Closing broker channel");
		r = amqp_channel_close(
			user->broker_conn, 1, AMQP_REPLY_SUCCESS);
		if (r.reply_type != AMQP_RESPONSE_NORMAL) {
			char buffer[512];
			openchange_broker_err(buffer, sizeof(buffer), r);
			i_error("openchange: Failed to close channel: %s",
				buffer);
		}

		i_debug("openchange: Closing broker connection");
		r = amqp_connection_close(
			user->broker_conn, AMQP_REPLY_SUCCESS);
		if (r.reply_type != AMQP_RESPONSE_NORMAL) {
			char buffer[512];
			openchange_broker_err(buffer, sizeof(buffer), r);
			i_error("openchange: Failed to close connection: %s",
				buffer);
		}

		ret = amqp_destroy_connection(user->broker_conn);
		if (ret != AMQP_STATUS_OK) {
			i_error("openchange: Failed to destroy broker connection: %s",
				amqp_error_string2(ret));
			return;
		}
	}
	user->broker_conn = NULL;
	user->broker_socket = NULL;
}


/**
 * Connect to broker
 */
	static bool
openchange_broker_connect(struct openchange_user *user)
{
	amqp_rpc_reply_t r;
	int ret;

	i_debug("openchange: Initializing broker connection");
	user->broker_conn = amqp_new_connection();
	if (user->broker_conn == NULL) {
		i_error("openchange: Failed to initialize broker connection");
		openchange_broker_disconnect(user);
		return FALSE;
	}

	i_debug("openchange: Initializing TCP socket");
	user->broker_socket = amqp_tcp_socket_new(user->broker_conn);
	if (user->broker_socket == NULL) {
		i_error("openchange: Failed to initialize TCP socket");
		openchange_broker_disconnect(user);
		return FALSE;
	}

	i_debug("openchange: Connecting to broker %s:%u",
			user->broker_host, user->broker_port);
	ret = amqp_socket_open(user->broker_socket,
			user->broker_host, user->broker_port);
	if (ret != AMQP_STATUS_OK) {
		i_error("openchange: Failed to connect to broker: %s",
				amqp_error_string2(ret));
		openchange_broker_disconnect(user);
		return FALSE;
	}

	i_debug("openchange: Logging into broker");
	r = amqp_login(user->broker_conn,
			"/",				/* Virtual host */
			AMQP_DEFAULT_MAX_CHANNELS,
			AMQP_DEFAULT_FRAME_SIZE,
			0,				/* Hearbeat */
			AMQP_SASL_METHOD_PLAIN,
			user->broker_user,
			user->broker_pass);
	if (r.reply_type != AMQP_RESPONSE_NORMAL) {
		char buffer[512];
		openchange_broker_err(buffer, sizeof(buffer), r);
		i_error("openchange: Failed to log in: %s", buffer);
		openchange_broker_disconnect(user);
		return FALSE;
	}

	i_debug("openchange: Opening broker channel");
	amqp_channel_open(user->broker_conn, 1);
	r = amqp_get_rpc_reply(user->broker_conn);
	if (r.reply_type != AMQP_RESPONSE_NORMAL) {
		char buffer[512];
		openchange_broker_err(buffer, sizeof(buffer), r);
		i_error("openchange: Failed to open channel: %s", buffer);
		openchange_broker_disconnect(user);
		return FALSE;
	}

	return TRUE;
}

/**
 * Publish a new message, JSON serialized.
 */
	static bool
openchange_publish(const struct openchange_user *user, const struct openchange_message *msg)
{
	amqp_rpc_reply_t r;
	int ret;

	if (user->broker_conn != NULL) {
		json_object *jobj, *juser, *jfolder, *juid;

		jobj = json_object_new_object();
		juser = json_object_new_string(user->username);
		jfolder = json_object_new_string(msg->destination_folder);
		juid = json_object_new_int(msg->uid);

		json_object_object_add(jobj, "user", juser);
		json_object_object_add(jobj, "folder", jfolder);
		json_object_object_add(jobj, "uid", juid);

		ret = amqp_basic_publish(user->broker_conn,
			1,
			amqp_cstring_bytes(user->broker_exchange),
			amqp_cstring_bytes(user->broker_routing),
			0,	/* Mandatory */
			0,	/* Inmediate */
			NULL,	/* Properties */
			amqp_cstring_bytes(json_object_to_json_string(jobj)));
		if (ret != AMQP_STATUS_OK) {
			i_error("openchange: Failed to publish: %s",
				amqp_error_string2(ret));
			return FALSE;
		}

		/* Free memory */
		json_object_put(jobj);
	}

	return TRUE;
}

/**
 * A new mail user was created. It doesn't yet have any namespaces.
 *
 */
	static void
openchange_mail_user_created(struct mail_user *user)
{
	struct openchange_user *ocuser;
	const char *str;

	i_debug("openchange: %s enter", __func__);
	ocuser = p_new(user->pool, struct openchange_user, 1);
	MODULE_CONTEXT_SET(user, openchange_user_module, ocuser);

	ocuser->fields = OPENCHANGE_DEFAULT_FIELDS;
	ocuser->events = OPENCHANGE_DEFAULT_EVENTS;
	ocuser->username = p_strdup(user->pool, user->username);

	/* Read plugin configuration and store in module context */
	str = mail_user_plugin_getenv(user, "openchange_broker_host");
	if (str == NULL) {
		i_error("openchange: Missing openchange_broker_host parameter");
	}
	ocuser->broker_host = str;

	str = mail_user_plugin_getenv(user, "openchange_broker_port");
	if (str == NULL) {
		i_error("openchange: Missing openchange_broker_port parameter");
	}
	if (str_to_uint(str, &ocuser->broker_port) < 0) {
		i_error("openchange: Invalid openchange_broker_port value");
	}

	str = mail_user_plugin_getenv(user, "openchange_broker_user");
	if (str == NULL) {
		i_error("openchange: Missing openchange_broker_user parameter");
	}
	ocuser->broker_user = str;

	str = mail_user_plugin_getenv(user, "openchange_broker_pass");
	if (str == NULL) {
		i_error("openchange: Missing openchange_broker_pass parameter");
	}
	ocuser->broker_pass = str;

	str = mail_user_plugin_getenv(user, "openchange_broker_exchange");
	if (str == NULL) {
		i_error("openchange: Missing openchange_broker_exchange parameter");
	}
	ocuser->broker_exchange = str;

	str = mail_user_plugin_getenv(user, "openchange_broker_routing_key");
	if (str == NULL) {
		i_error("openchange: Missing openchange_broker_routing_key parameter");
	}
	ocuser->broker_routing = str;
	i_debug("openchange: %s exit", __func__);
}

	static void
openchange_mail_save(void *txn, struct mail *mail)
{
	i_debug("openchange: %s enter", __func__);
	i_debug("openchange: %s exit", __func__);
}

	static void
openchange_mail_copy(void *txn, struct mail *src, struct mail *dst)
{
	struct openchange_mail_txn_context *ctx;
	struct openchange_message *msg;
	struct openchange_user *ocuser;
	int i;

	i_debug("openchange: %s enter", __func__);
	ctx = (struct openchange_mail_txn_context *) txn;
	ocuser = OPENCHANGE_USER_CONTEXT(dst->box->storage->user);
	if (strcmp(src->box->storage->name, "raw") == 0) {
		/* special case: lda/lmtp is saving a mail */
		msg = p_new(ctx->pool, struct openchange_message, 1);
		msg->event = OPENCHANGE_EVENT_COPY;
		msg->uid = 0;
		msg->destination_folder = p_strdup(ctx->pool, mailbox_get_name(dst->box));
		DLLIST2_APPEND(&ctx->messages, &ctx->messages_tail, msg);
	}
	i_debug("openchange: %s exit", __func__);
}

	static void *
openchange_mail_transaction_begin(struct mailbox_transaction_context *t)
{
	struct openchange_mail_txn_context *ctx;
	pool_t pool;

	i_debug("openchange: %s enter", __func__);
	pool = pool_alloconly_create("openchange", 2048);
	ctx = p_new(pool, struct openchange_mail_txn_context, 1);
	ctx->pool = pool;
	ctx->ns = mailbox_get_namespace(t->box);
	i_debug("openchange: %s exit", __func__);

	return ctx;
}

	static void
openchange_mail_transaction_commit(void *txn, struct mail_transaction_commit_changes *changes)
{
	struct openchange_mail_txn_context *ctx;
	struct openchange_message *msg;
	struct openchange_user *ocuser;
	struct seq_range_iter iter;
	unsigned int n = 0;
	uint32_t uid;

	i_debug("openchange: %s enter", __func__);
	ctx = (struct openchange_mail_txn_context *)txn;
	ocuser = OPENCHANGE_USER_CONTEXT(ctx->ns->user);

	/* Open connection to broker */
	if (!openchange_broker_connect(ocuser)) {
		pool_unref(&ctx->pool);
		return;
	}

	/* Send notifications for new mails */
	seq_range_array_iter_init(&iter, &changes->saved_uids);
	for (msg = ctx->messages; msg != NULL; msg = msg->next) {
		if (msg->event == OPENCHANGE_EVENT_COPY) {
			if (seq_range_array_iter_nth(&iter, n++, &uid)) {
				msg->uid = uid;
				i_debug("openchange: Message username: %s, Message folder: %s, Message uid: %u",
					ocuser->username, msg->destination_folder, msg->uid);
				openchange_publish(ocuser, msg);
			}
		}
	}

	i_assert(!seq_range_array_iter_nth(&iter, n, &uid));

	/* Close connection to broker */
	openchange_broker_disconnect(ocuser);

	pool_unref(&ctx->pool);
	i_debug("openchange: %s exit", __func__);
}

	static void
openchange_mail_transaction_rollback(void *txn)
{
	struct openchange_mail_txn_context *ctx;

	i_debug("openchange: %s enter", __func__);
	ctx = (struct openchange_mail_txn_context *)txn;
	pool_unref(&ctx->pool);
	i_debug("openchange: %s exit", __func__);
}

static const struct notify_vfuncs openchange_vfuncs = {
	.mail_transaction_begin = openchange_mail_transaction_begin,
	.mail_save = openchange_mail_save,
	.mail_copy = openchange_mail_copy,
	.mail_transaction_commit = openchange_mail_transaction_commit,
	.mail_transaction_rollback = openchange_mail_transaction_rollback,
};

static struct mail_storage_hooks openchange_mail_storage_hooks = {
	.mail_user_created = openchange_mail_user_created,
};

	void
openchange_plugin_init(struct module *module)
{
	i_debug("openchange: %s enter", __func__);
	openchange_ctx = notify_register(&openchange_vfuncs);
	mail_storage_hooks_add(module, &openchange_mail_storage_hooks);
	i_debug("openchange: %s exit", __func__);
}

	void
openchange_plugin_deinit(void)
{
	i_debug("openchange: %s enter", __func__);
	mail_storage_hooks_remove(&openchange_mail_storage_hooks);
	notify_unregister(openchange_ctx);
	i_debug("openchange: %s exit", __func__);
}

