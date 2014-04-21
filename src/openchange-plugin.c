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
#include <dovecot/notify-plugin.h>
#include <dovecot/mail-storage-hooks.h>
#include <dovecot/mail-storage.h>
#include <dovecot/module-context.h>

const char *openchange_plugin_version = DOVECOT_VERSION;
const char *openchange_plugin_dependencies[] = { "notify", NULL };
struct notify_context *openchange_ctx;

	static void
openchange_mail_user_created(struct mail_user *user)
{
	i_debug("%s: username = %s\n", __func__, user->username);
}

	static void
openchange_mail_save(void *txn, struct mail *mail)
{
	i_debug("%s: message UID = %d\n", __func__, mail->uid);
}

	static void
openchange_mail_copy(void *txn, struct mail *src, struct mail *dst)
{
	i_debug("%s\n", __func__);
}

	static void *
openchange_mail_transaction_begin(struct mailbox_transaction_context *t ATTR_UNUSED)
{
	i_debug("%s\n", __func__);
	return NULL;
}

	static void
openchange_mail_transaction_commit(void *txn, struct mail_transaction_commit_changes *changes)
{
	i_debug("%s\n", __func__);
}

	static void
openchange_mail_transaction_rollback(void *txn)
{
	i_debug("%s\n", __func__);
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
	i_debug("%s\n", __func__);
	openchange_ctx = notify_register(&openchange_vfuncs);
	mail_storage_hooks_add(module, &openchange_mail_storage_hooks);
}

	void
openchange_plugin_deinit(void)
{
	i_debug("%s\n", __func__);
	mail_storage_hooks_remove(&openchange_mail_storage_hooks);
	notify_unregister(openchange_ctx);
}

