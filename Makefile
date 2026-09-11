# Retro-Go SD — Duren (Durak) GWHB homebrew
#
#   make                  — build + pack GWHB (PROJECT_KIND=homebrew)
#   make docker
#   make host             — native SDL2 desktop build (./Duren_host)
#   make host_run         — build and launch
#   make resources        — rebuild ./Duren.pak (next to Duren.bin)
#
# Build outputs (repo root):
#   Duren.bin             — GWHB binary
#   Duren.pak             — combined gfx+audio+lang resources
#
# On the SD card, both go under /homebrews/ (release zip lays this out).
# Host assets: host_data/ (PNG atlases + copy of Duren.pak).
#
# Verbose compiler lines: make V=

#######################################
# Project identity
#######################################
PROJECT_KIND ?= homebrew

CORE_NAME  := duren
CORE_ENTRY := app_main

CORE_C_SOURCES := \
src/main.c \
src/hb_compat.c \
src/duren/duren_pak.c \
src/duren/hal_retro_go.c \
src/duren/duren_game.c \
src/duren/game.c \
src/duren/modes.c \
src/duren/tournament.c \
src/duren/ui_modes.c \
src/duren/battle_royal.c \
src/duren/battle_royal_ui.c \
src/duren/savegame.c \
src/duren/rng.c \
src/duren/dialogue_data.c \
src/duren/career_map.c \
src/duren/career_progress.c \
src/duren/assets.c \
src/duren/audio_assets.c

CORE_C_INCLUDES := \
-Isrc/duren

# Local libm is not required — rotation uses cosf/sqrtf (ABI) + local sinf/roundf.
# CORE_LDLIBS := -lm

GNW_CORE_SDK ?= sdk
BUILD_DIR ?= build/$(PROJECT_KIND)

#######################################
# Kind-specific compile defs + packing
#######################################
ifeq ($(PROJECT_KIND),core)
$(error This project is a homebrew only — use PROJECT_KIND=homebrew)

else ifeq ($(PROJECT_KIND),homebrew)
CORE_C_DEFS := \
-DPROJECT_KIND_HOMEBREW=1 \
-DPLATFORM_RETRO_GO=1

PACKED_BIN := Duren.bin
HB_NAME    := Duren
COVER_JPG  := $(BUILD_DIR)/cover.jpg
COVER_WIDTH  ?= 128
COVER_HEIGHT ?= 96

else
$(error PROJECT_KIND must be 'homebrew' (got '$(PROJECT_KIND)'))
endif

include $(GNW_CORE_SDK)/Makefile

PACK_HOMEBREW := $(GNW_CORE_SDK)/tools/pack_homebrew.py
PACK_DUREN_PAK := scripts/pack_duren_pak.py
# Built next to Duren.bin at the repo root (also shipped in the release zip).
DUREN_PAK_OUT := Duren.pak
DUREN_PAK_GFX := sd_content/roms/homebrew/duren_gfx.pak
DUREN_PAK_AUDIO := sd_content/roms/homebrew/duren_audio.pak
DUREN_PAK_LANG := sd_content/roms/homebrew/duren_lang.pak
# Extra files staged under /homebrews/ in the GitHub release install zip.
RELEASE_EXTRAS := $(DUREN_PAK_OUT)
# The shared dist scripts read SIDECARS. Same list, our spelling; upstream's
# name stays the source of truth so merging from it needs no edit here.
SIDECARS       := $(RELEASE_EXTRAS)
# Published full size beside the release; the packed cover derives from it.
COVER_FULL     := src/assets/cover_src.png

#######################################
# Packed header version
#######################################
CORE_VERSION ?= $(shell git describe --tags --dirty 2>/dev/null || echo NOTAG)

#######################################
# Pack
#######################################
.PHONY: pack cover resources

cover: $(COVER_JPG)

resources: $(DUREN_PAK_OUT)

$(DUREN_PAK_OUT): $(PACK_DUREN_PAK) $(DUREN_PAK_GFX) $(DUREN_PAK_AUDIO) $(DUREN_PAK_LANG)
	$(V)$(ECHO) "[ PACK ]" $(DUREN_PAK_OUT)
	$(V)python3 $(PACK_DUREN_PAK) \
		--gfx $(DUREN_PAK_GFX) \
		--audio $(DUREN_PAK_AUDIO) \
		--lang $(DUREN_PAK_LANG) \
		--out $(DUREN_PAK_OUT)
	$(V)mkdir -p host_data
	$(V)cp -f $(DUREN_PAK_OUT) host_data/Duren.pak

$(COVER_JPG): src/assets/cover_src.png
	$(V)$(ECHO) "[ COVER ]" $(COVER_JPG)
	$(V)mkdir -p $(BUILD_DIR)
	$(V)python3 -c "from pathlib import Path; from PIL import Image; \
img=Image.open('src/assets/cover_src.png').convert('RGB'); \
img.thumbnail(($(COVER_WIDTH),$(COVER_HEIGHT))); \
canvas=Image.new('RGB', ($(COVER_WIDTH),$(COVER_HEIGHT)), (8,42,29)); \
x=($(COVER_WIDTH)-img.width)//2; y=($(COVER_HEIGHT)-img.height)//2; \
canvas.paste(img, (x,y)); \
canvas.save('$(COVER_JPG)', 'JPEG', quality=80, optimize=True); \
sz=Path('$(COVER_JPG)').stat().st_size; \
assert sz <= 10*1024, f'cover too big: {sz}'"

pack: $(TARGET_BIN) $(COVER_JPG) $(DUREN_PAK_OUT)
	$(V)$(ECHO) [ PACK GWHB ] $(PACKED_BIN) version=$(CORE_VERSION)
	$(V)python3 $(PACK_HOMEBREW) \
		--elf $(TARGET_ELF) --bin $(TARGET_BIN) \
		--name "$(HB_NAME)" --version "$(CORE_VERSION)" \
		--cover $(COVER_JPG) \
		--out $(PACKED_BIN)

all: pack

.PHONY: print-PROJECT_KIND print-PACKED_BIN print-SIDECARS print-RO_BIN print-CORE_NAME \
	print-COVER_FULL print-DOCKER_IMAGE \
	print-TARGET_ELF print-TARGET_MAP print-CORE_VERSION print-RELEASE_EXTRAS
print-PROJECT_KIND:
	@echo $(PROJECT_KIND)
print-PACKED_BIN:
	@echo $(PACKED_BIN)
# Extra device files installed beside PACKED_BIN, space separated: here the
# 5.6 MiB resource pack the game opens from its own folder. RO_BIN is the older
# single-slot spelling, read for every project so the shared stage_release.py
# needs no per-project variant.
print-SIDECARS:
	@echo $(SIDECARS)
print-RO_BIN:
	@echo $(RO_BIN)
print-COVER_FULL:
	@echo $(COVER_FULL)
print-CORE_NAME:
	@echo $(CORE_NAME)
print-DOCKER_IMAGE:
	@echo $(DOCKER_IMAGE)
print-TARGET_ELF:
	@echo $(TARGET_ELF)
print-TARGET_MAP:
	@echo $(BUILD_DIR)/$(CORE_NAME)_core.map
print-CORE_VERSION:
	@echo $(CORE_VERSION)
print-RELEASE_EXTRAS:
	@echo $(RELEASE_EXTRAS)

clean::
	$(V)rm -f $(PACKED_BIN) $(DUREN_PAK_OUT)
	$(V)rm -f $(COVER_JPG)

#######################################
# Docker
#######################################
.PHONY: docker docker_pull docker_shell

RELEASE_VERSION ?= v1.5
DOCKER_REPOSITORY ?= sylverb/retro-go-sd-builder
DOCKER_IMAGE ?= $(DOCKER_REPOSITORY):$(RELEASE_VERSION)

DOCKER_TTY_FLAG := $(shell if [ -t 0 ]; then echo -it; else echo; fi)
DOCKER_USER := $(shell id -u):$(shell id -g)
DOCKER_RUN := docker run --rm $(DOCKER_TTY_FLAG) \
	--user $(DOCKER_USER) \
	-v "$(CURDIR):/opt/workdir" \
	-w /opt/workdir \
	$(DOCKER_IMAGE)

docker:
	$(V)$(ECHO) "[ DOCKER ]" $(DOCKER_IMAGE) "PROJECT_KIND=$(PROJECT_KIND)"
	$(V)$(DOCKER_RUN) make --no-print-directory -j$$(nproc) PROJECT_KIND=$(PROJECT_KIND)

docker_pull:
	$(V)$(ECHO) "[ PULL ]" $(DOCKER_IMAGE)
	$(V)docker pull $(DOCKER_IMAGE)

docker_shell:
	$(DOCKER_RUN) bash

#######################################
# Host SDL (native Duren desktop build)
#######################################
include host/Makefile.host
