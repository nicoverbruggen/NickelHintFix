include NickelHook/NickelHook.mk

override LIBRARY  := src/libnickelhintfix.so
override SOURCES  += src/config.c src/nickelhintfix.cc

# The vertical-text fix uses QString (KepubBookReader::pageStyleCss returns QString and
# writingDirectionFromString takes QString const&), so link Qt5Core. NickelHook.mk turns
# PKGCONF entries into the right -I/-l flags from the nickeltc sysroot.
override PKGCONF  += Qt5Core

override CFLAGS   += -Wall -Wextra -Werror -fvisibility=hidden
override CXXFLAGS += -std=gnu++11 -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden -fvisibility-inlines-hidden
override KOBOROOT += res/doc:$(NHF_CONFIG_DIR)/doc res/default:$(NHF_CONFIG_DIR)/default res/uninstall:$(NHF_CONFIG_DIR)/uninstall

override SKIPCONFIGURE += strip
strip:
	$(STRIP) --strip-unneeded src/libnickelhintfix.so
.PHONY: strip

ifeq ($(NHF_CONFIG_DIR),)
override NHF_CONFIG_DIR := /mnt/onboard/.adds/nickelhintfix
endif

override CPPFLAGS += -DNHF_CONFIG_DIR='"$(NHF_CONFIG_DIR)"' -DNHF_CONFIG_DIR_DISP='"$(patsubst /mnt/onboard/%,KOBOeReader/%,$(NHF_CONFIG_DIR))"'

include NickelHook/NickelHook.mk
