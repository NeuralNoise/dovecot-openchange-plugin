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
#include <dovecot/lib.h>
#include <dovecot/llist.h>
#include <dovecot/mail-user.h>
#include <dovecot/mail-storage-hooks.h>
#include <dovecot/mail-storage.h>
#include <dovecot/mail-storage-private.h>
#include <dovecot/module-context.h>
#include <dovecot/notify-plugin.h>

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
	char				*username;
};

struct openchange_message {
	struct openchange_message	*prev;
	struct openchange_message	*next;
	enum openchange_event		event;
	//bool				ignore;
	uint32_t			uid;
	char				*destination_folder;
	char				*username;
};

struct openchange_mail_txn_context {
	pool_t pool;
	struct openchange_message *messages;
	struct openchange_message *messages_tail;
};

static MODULE_CONTEXT_DEFINE_INIT(openchange_user_module,
				  &mail_user_module_register);

/**
 * Publish a new message
 */
	static void
openchange_publish(const struct openchange_message *msg)
{
}

/**
 * A new mail user was created. It doesn't yet have any namespaces.
 *
 */
	static void
openchange_mail_user_created(struct mail_user *user)
{
	struct openchange_user *ocuser;
	//const char *str;

	i_debug("%s: enter", __func__);
	ocuser = p_new(user->pool, struct openchange_user, 1);
	MODULE_CONTEXT_SET(user, openchange_user_module, ocuser);

	ocuser->fields = OPENCHANGE_DEFAULT_FIELDS;
	ocuser->events = OPENCHANGE_DEFAULT_EVENTS;
	ocuser->username = p_strdup(user->pool, user->username);
	//str = mail_user_plugin_getenv(user, "ocsmanager_backend");
	//if (!str) {
	//	i_fatal("Missing ocsmanager_backend parameter in dovecot.conf");
	//}
	//ocsuser->backend = str;
	//str = mail_user_plugin_getenv(user, "ocsmanager_newmail");
	//if (!str) {
	//	i_fatal("Missing ocsmanager_newmail parameter in dovecot.conf");
	//}
	//ocsuser->bin = str;
	//str = mail_user_plugin_getenv(user, "ocsmanager_config");
	//if (!str) {
	//	i_fatal("Missing ocsmanager_config parameter in dovecot.conf");
	//}
	//ocsuser->config = str;
	i_debug("%s: exit", __func__);
}

	static void
openchange_mail_save(void *txn, struct mail *mail)
{
	i_debug("%s: enter", __func__);
	i_debug("%s: exit", __func__);
}

	static void
openchange_mail_copy(void *txn, struct mail *src, struct mail *dst)
{
	struct openchange_mail_txn_context *ctx;
	struct openchange_message *msg;
	struct openchange_user *mctx;
	int i;

	i_debug("%s: enter", __func__);
	ctx = (struct openchange_mail_txn_context *) txn;
	mctx = OPENCHANGE_USER_CONTEXT(dst->box->storage->user);
	if (strcmp(src->box->storage->name, "raw") == 0) {
		/* special case: lda/lmtp is saving a mail */
		msg = p_new(ctx->pool, struct openchange_message, 1);
		msg->event = OPENCHANGE_EVENT_COPY;
		msg->uid = 0;
		msg->username = p_strdup(ctx->pool, mctx->username);
		msg->destination_folder = p_strdup(ctx->pool, mailbox_get_name(dst->box));
		//msg->ignore = FALSE;
		//msg->backend = p_strdup(ctx->pool, mctx->backend);
		//msg->bin = p_strdup(ctx->pool, mctx->bin);
		//msg->config = p_strdup(ctx->pool, mctx->config);

		/* FIXME: Quick hack of the night */
		//msg->username[0] = toupper(msg->username[0]);
		//for (i = 0; i < strlen(msg->destination_folder); i++) {
		//	msg->destination_folder[i] = tolower(msg->destination_folder[i]);
		//}
		DLLIST2_APPEND(&ctx->messages, &ctx->messages_tail, msg);
	}
	i_debug("%s: exit", __func__);
}

	static void *
openchange_mail_transaction_begin(struct mailbox_transaction_context *t ATTR_UNUSED)
{
	struct openchange_mail_txn_context *ctx;
	pool_t pool;

	i_debug("%s: enter", __func__);
	pool = pool_alloconly_create("openchange", 2048);
	ctx = p_new(pool, struct openchange_mail_txn_context, 1);
	ctx->pool = pool;
	i_debug("%s: exit", __func__);

	return ctx;
}

	static void
openchange_mail_transaction_commit(void *txn, struct mail_transaction_commit_changes *changes)
{
	struct openchange_mail_txn_context *ctx;
	struct openchange_message *msg;
	struct seq_range_iter iter;
	unsigned int n = 0;
	uint32_t uid;

	i_debug("%s: enter", __func__);
	ctx = (struct openchange_mail_txn_context *)txn;
	seq_range_array_iter_init(&iter, &changes->saved_uids);
	for (msg = ctx->messages; msg != NULL; msg = msg->next) {
		i_debug("%s: Message event: 0x%02X", __func__, msg->event);
		if (msg->event == OPENCHANGE_EVENT_COPY) {
		    // XXX || msg->event == OPENCHANGE_EVENT_SAVE) {
			if (seq_range_array_iter_nth(&iter, n++, &uid)) {
				msg->uid = uid;
				i_debug("%s: Message username: %s, Message folder: %s, Message uid: %u",
					__func__, msg->username, msg->destination_folder, msg->uid);
				openchange_publish(msg);
			}
		}
	}
	i_assert(!seq_range_array_iter_nth(&iter, n, &uid));
	pool_unref(&ctx->pool);
	i_debug("%s: exit", __func__);
}

	static void
openchange_mail_transaction_rollback(void *txn)
{
	struct openchange_mail_txn_context *ctx;

	i_debug("%s: enter", __func__);
	ctx = (struct openchange_mail_txn_context *)txn;
	pool_unref(&ctx->pool);
	i_debug("%s: exit", __func__);
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
	i_debug("%s: enter", __func__);
	openchange_ctx = notify_register(&openchange_vfuncs);
	mail_storage_hooks_add(module, &openchange_mail_storage_hooks);
	i_debug("%s: exit", __func__);
}

	void
openchange_plugin_deinit(void)
{
	i_debug("%s: enter", __func__);
	mail_storage_hooks_remove(&openchange_mail_storage_hooks);
	notify_unregister(openchange_ctx);
	i_debug("%s: exit", __func__);
}

