AM_CPPFLAGS = $(LIBDOVECOT_INCLUDE) \
              $(LIBDOVECOT_STORAGE_INCLUDE) \
              $(LIBDOVECOT_IMAP_INCLUDE) \
              $(RABBITMQ_INCLUDE) \
              $(RABBITMQ_CFLAGS) \
              $(JSONC_INCLUDE) \
              $(JSONC_CFLAGS)

LIBS += $(RABBITMQ_LIBS) \
        $(JSONC_LIBS)

openchange_moduledir = $(dovecot_moduledir)
openchange_module_LTLIBRARIES = lib90_openchange_plugin.la

lib90_openchange_plugin_la_LDFLAGS = -module -avoid-version -shared
lib90_openchange_plugin_la_SOURCES = openchange-plugin.c

noinst_HEADERS = openchange-plugin.h
