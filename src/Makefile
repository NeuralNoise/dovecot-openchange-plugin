# TODO Add copyright header

# Dovecot's IMAP plugin path
DOVECOT_MODULES_PATH = /usr/lib/dovecot/modules

PLUGIN_SOURCES = openchange-plugin.c
PLUGIN_NAME = lib90_openchange_plugin.so

.PHONY: all build install clean

all: build

build: ${PLUGIN_NAME}

${PLUGIN_NAME}: ${PLUGIN_SOURCES}
	gcc  -fPIC -shared -DHAVE_CONFIG_H openchange-plugin.c -o ${PLUGIN_NAME} -lrabbitmq -ljson-c

install: install_plugin

install_plugin: ${PLUGIN_NAME}
	install -d ${DOVECOT_MODULES_PATH}
	install -m644 $< ${DOVECOT_MODULES_PATH}
	ldconfig /usr/lib/dovecot/modules/

clean:
	$(RM) ${PLUGIN_NAME}
