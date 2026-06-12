#---------------------------------------------------------------------------------
# jellyfin-3ds - Native Jellyfin client for Nintendo 3DS
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# Project configuration
#---------------------------------------------------------------------------------
TARGET		:=	jellyfin-3ds
BUILD		:=	build
SOURCES		:=	src src/api src/audio src/video src/ui src/util
DATA		:=	data
INCLUDES	:=	include include/api include/audio include/video include/ui include/util

APP_TITLE		:= Jellyfin 3DS
APP_DESCRIPTION	:= Jellyfin media client for Nintendo 3DS
APP_AUTHOR		:= jellyfin-3ds team

# Icon — fall back to libctru default if ours doesn't exist yet
ifneq ($(wildcard $(TOPDIR)/assets/icons/icon.png),)
ICON		:=	assets/icons/icon.png
else
ICON		:=
endif

#---------------------------------------------------------------------------------
# Options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-ffunction-sections \
			$(ARCH)

# Version is derived from git tags so the binary, UI header, MediaBrowser
# auth header, and releases can never drift apart. Tagged commit -> "1.1.0";
# between tags -> "1.0.1-8-gabc1234"; dirty tree gets "-dirty"; no git/tags
# (tarball build) -> "unknown". Override with: make JFIN_VERSION=x.y.z
GIT_DESC := $(shell git describe --tags --always --dirty 2>/dev/null)
JFIN_VERSION ?= $(if $(GIT_DESC),$(patsubst v%,%,$(GIT_DESC)),unknown)

CFLAGS	+=	$(INCLUDE) -D__3DS__ -DJFIN_VERSION=\"$(JFIN_VERSION)\"

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# FFmpeg static libraries (built via lib/ffmpeg/build-ffmpeg.sh)
FFMPEG_DIR	:= $(TOPDIR)/lib/ffmpeg

LIBS	:= -lcitro2d -lcitro3d \
		   -lavformat -lavcodec -lavfilter -lswresample -lavutil \
		   -lcurl -lmbedtls -lmbedx509 -lmbedcrypto \
		   -lmpg123 -lopusfile -lopus -lvorbisidec -logg \
		   -lz -lctru -lm

LIBDIRS	:= $(FFMPEG_DIR) $(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
# Build rules — first invocation sets up, then recurses into $(BUILD)/
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD	:=	$(CC)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES	:=	$(OFILES_BIN) $(OFILES_SOURCES)
export HFILES	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					-I$(FFMPEG_DIR)/include \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	-L$(FFMPEG_DIR) $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
	export APP_ICON := $(DEVKITPRO)/libctru/default_icon.png
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

.PHONY: all clean

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# Main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(OUTPUT).smdh

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
# Binary data rules
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
