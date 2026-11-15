ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not set. Install devkitPro 3DS development tools first.)
endif

include $(DEVKITARM)/3ds_rules

TARGET      := ShieldOnlineArt
BUILD       := build
SOURCES     := source
INCLUDES    := include
DATA        :=
GRAPHICS    :=
ROMFS       := romfs
ROMFS_FILES := $(shell find $(ROMFS) -type f)

APP_VERSION     := v0.1.9
# Packed CIA title version: (major << 10) | (minor << 4) | micro.
# Use -ver explicitly: makerom 0.19.0 ignored the separate minor argument here.
CIA_VERSION     := 25
# Keep the fixed packager local to this project; do not use makerom 0.19.0,
# whose command-line version macros corrupt the minor title-version field.
MAKEROM         ?= makerom
BANNERTOOL      ?= bannertool
APP_TITLE       := SHIELD Online Art $(APP_VERSION)
APP_DESCRIPTION := Galerie en ligne experimentale pour 3DS
APP_AUTHOR      := Yanis

ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS   := -g -Wall -Wextra -O2 -mword-relocations -ffunction-sections -fdata-sections -fstack-usage $(ARCH)
CFLAGS   += -I$(DEVKITPRO)/libctru/include -I$(DEVKITPRO)/portlibs/3ds/include -I$(INCLUDES) -D__3DS__
LDFLAGS  := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections,-Map,$(BUILD)/$(TARGET).map
LIBS     := -L$(DEVKITPRO)/libctru/lib -L$(DEVKITPRO)/portlibs/3ds/lib -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -ljpeg -lpng -lz -lctru -lm

.PHONY: all cia clean
all: cia

cia: $(BUILD)/$(TARGET).cia

$(BUILD)/$(TARGET).cia: $(BUILD)/$(TARGET).elf $(ROMFS_FILES) Makefile app.rsf icon.png banner.png assets/silence.wav
	@mkdir -p $(BUILD)
	@"$(BANNERTOOL)" makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i icon.png -o $(BUILD)/icon.icn
	@"$(BANNERTOOL)" makebanner -i banner.png -a assets/silence.wav -o $(BUILD)/banner.bnr
	@"$(MAKEROM)" -f cia -o $@.tmp -target t -ver $(CIA_VERSION) -desc app:4 -rsf app.rsf -exefslogo -elf $(BUILD)/$(TARGET).elf -icon $(BUILD)/icon.icn -banner $(BUILD)/banner.bnr
	@mv -f $@.tmp $@

$(BUILD)/$(TARGET).elf: $(SOURCES)/main.c $(SOURCES)/art_content.c $(SOURCES)/art_page.c $(SOURCES)/oauth_pkce.c $(SOURCES)/oauth_session.c $(SOURCES)/oauth_exchange.c $(SOURCES)/oauth_transport.c $(SOURCES)/oauth_token.c $(SOURCES)/auth_qr.c $(SOURCES)/auth_lan.c $(SOURCES)/auth_listener.c third_party/qrcodegen/qrcodegen.c $(INCLUDES)/oauth_token.h $(INCLUDES)/oauth_transport.h $(INCLUDES)/oauth_exchange.h $(INCLUDES)/auth_listener.h $(INCLUDES)/auth_lan.h $(INCLUDES)/auth_qr.h $(INCLUDES)/deviantart_config.h $(INCLUDES)/art_content.h $(INCLUDES)/art_page.h $(INCLUDES)/oauth_pkce.h $(INCLUDES)/oauth_session.h $(DEVKITPRO)/portlibs/3ds/include/jpeglib.h $(DEVKITPRO)/portlibs/3ds/include/png.h $(DEVKITPRO)/portlibs/3ds/include/mbedtls/sha256.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES)/main.c $(SOURCES)/art_content.c $(SOURCES)/art_page.c $(SOURCES)/oauth_pkce.c $(SOURCES)/oauth_session.c $(SOURCES)/oauth_exchange.c $(SOURCES)/oauth_transport.c $(SOURCES)/oauth_token.c $(SOURCES)/auth_qr.c $(SOURCES)/auth_lan.c $(SOURCES)/auth_listener.c third_party/qrcodegen/qrcodegen.c $(LIBS)

clean:
	@rm -rf $(BUILD)
