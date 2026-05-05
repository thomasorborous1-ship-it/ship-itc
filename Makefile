PS2DEV ?= /opt/ps2dev
PS2SDK ?= $(PS2DEV)/ps2sdk
GSKIT  ?= $(PS2DEV)/gsKit

SRC_DIR := $(CURDIR)/src
OBJ_DIR := $(CURDIR)/build
PKG_DIR := $(OBJ_DIR)/pkg
EMBED_DIR := $(OBJ_DIR)/embed
TARGET  := $(OBJ_DIR)/SNESticle.elf
BIN2C   ?= $(PS2SDK)/bin/bin2c

EE_CC  ?= $(shell command -v ee-gcc 2>/dev/null || command -v mips64r5900el-ps2-elf-gcc 2>/dev/null)
EE_CXX ?= $(shell command -v ee-g++ 2>/dev/null || command -v mips64r5900el-ps2-elf-g++ 2>/dev/null)
EE_STRIP ?= ee-strip

IRX_DIR     ?= $(PS2SDK)/iop/irx

# DEBUG_BOOT_SCREEN: when set to 1 the EE side calls init_scr() in main()
# and every "[boot] ..." trace point goes to scr_printf() (BIOS debug
# font, written direct to the GS, no IOP/SIF/host: round-trip needed).
# In that mode MainLoopInit() also skips its own GS_InitGraph() so the
# debug screen survives long enough for the user to read it. Set to 0
# for normal rendering. Override on the make line if needed.
DEBUG_BOOT_SCREEN ?= 0

# MAINLOOP_DEBUG_GS_TEST: when set to 1, MainLoopRender() paints the
# whole frame solid red as the first thing every frame. Use to confirm
# whether the GS pipeline itself is alive (red = OK, still black = GS
# broken). Override on the make line: `make MAINLOOP_DEBUG_GS_TEST=1`.
MAINLOOP_DEBUG_GS_TEST ?= 0

CFLAGS := -G0 -O2 -Wall -fno-strict-aliasing \
	-D_EE -DPS2 -DLSB_FIRST -DALIGN_DWORD -DCODE_PLATFORM=3 \
	-DDEBUG_BOOT_SCREEN=$(DEBUG_BOOT_SCREEN) \
	-DMAINLOOP_DEBUG_GS_TEST=$(MAINLOOP_DEBUG_GS_TEST)

CXXFLAGS := -G0 -O2 -Wall -fno-strict-aliasing -Wno-narrowing -Wno-overflow -fno-exceptions -fno-rtti -fpermissive \
	-D_EE -DPS2 -DLSB_FIRST -DALIGN_DWORD -DCODE_PLATFORM=3 \
	-DDEBUG_BOOT_SCREEN=$(DEBUG_BOOT_SCREEN) \
	-DMAINLOOP_DEBUG_GS_TEST=$(MAINLOOP_DEBUG_GS_TEST)

INCS := \
	-I$(EMBED_DIR) \
	-I$(CURDIR)/src \
	-I$(CURDIR)/src/app \
	-I$(CURDIR)/src/common/base \
	-I$(CURDIR)/src/common/debug \
	-I$(CURDIR)/src/common/io \
	-I$(CURDIR)/src/common/media \
	-I$(CURDIR)/src/common/render \
	-I$(CURDIR)/src/modules/mcsave \
	-I$(CURDIR)/src/modules/netplay \
	-I$(CURDIR)/src/modules/sjpcm \
	-I$(CURDIR)/src/platform/ps2 \
	-I$(CURDIR)/src/platform/ps2/cdvd \
	-I$(CURDIR)/src/platform/ps2/common \
	-I$(CURDIR)/src/platform/ps2/gs \
	-I$(CURDIR)/src/platform/ps2/input \
	-I$(CURDIR)/src/platform/ps2/lowlevel \
	-I$(CURDIR)/src/platform/ps2/memcard \
	-I$(CURDIR)/src/platform/ps2/system \
	-I$(CURDIR)/src/platform/ps2/ui \
	-I$(CURDIR)/src/snes/apu \
	-I$(CURDIR)/src/snes/core \
	-I$(CURDIR)/src/snes/cpu \
	-I$(CURDIR)/src/snes/ppu \
	-I$(CURDIR)/src/snes/rom \
	-I$(CURDIR)/src/snes/state \
	-I$(CURDIR)/src/third_party/miniz \
	-I$(PS2SDK)/common/include \
	-I$(PS2SDK)/ee/include \
	-I$(PS2SDK)/ports/include \
	-I$(GSKIT)/include

LIBDIRS := \
	-L$(PS2SDK)/ee/lib \
	-L$(PS2SDK)/ports/lib \
	-L$(GSKIT)/lib

# gsKit + dmaKit must come before the SDK's libgraph, because
# gsKit pulls in DMA helpers from dmaKit and the linker resolves
# left-to-right. Linking order is also why -lkernel/-lc/-lm/-lstdc++
# is kept at the end.
LIBS := \
	-lgskit -ldmakit -lgskit_toolkit \
	-lmc -lpad -lps2ip \
	-laudsrv \
	-lpatches \
	-lkernel -lc -lm -lstdc++ -lgcc

SRCS := $(shell tr '\n' ' ' < ok-files.txt)

OBJS := \
	$(patsubst src/%.c,$(OBJ_DIR)/%.o,$(filter %.c,$(SRCS))) \
	$(patsubst src/%.cpp,$(OBJ_DIR)/%.o,$(filter %.cpp,$(SRCS))) \
	$(patsubst src/%.s,$(OBJ_DIR)/%.o,$(filter %.s,$(SRCS))) \
	$(patsubst src/%.S,$(OBJ_DIR)/%.o,$(filter %.S,$(SRCS)))

SDK_NET_IRX := ps2dev9.irx netman.irx ps2ip-nm.irx smap.irx
SDK_COMPAT_NET_IRX := ps2ip.irx ps2ips.irx smap-ps2ip.irx
SDK_MC_IRX := mcman.irx mcserv.irx
SDK_EXTRA_IRX := ioptrap.irx poweroff.irx

CUSTOM_IRX_DIR ?= $(CURDIR)/irx
CUSTOM_IRX := CDVD.IRX NETPLAY.IRX MCSAVE.IRX

# IRX modules embedded directly into the ELF via bin2c. The custom
# IRX search paths (host:, cdrom:) used by the original code do not
# work on emulators or stripped-down PS2 setups, so we ship these
# modules inside the executable and load them via SifExecModuleBuffer.
#
# Audio is provided by audsrv.irx from the PS2SDK
# ($(PS2SDK)/iop/irx/audsrv.irx). It replaces the legacy SJPCM2.IRX,
# whose RPC server was unreliable on modern IOPs / emulators - see
# src/modules/sjpcm/sjpcm_rpc.c for the EE-side wrapper.
#
# CDVD.IRX and MCSAVE.IRX are intentionally NOT embedded:
# on NetherSX2 (and likely any IOP that isn't a 100% faithful real PS2)
# the iaddis custom IRXs do load via SifExecModuleBuffer, but their
# RPC entry points either never come up or are incompatible with the
# rom-resident services. The result is the EE-side init function
# (CDVD_Init, MCSave_Init) spinning forever in SifBindRpc and
# deadlocking the boot. With these IRXs not embedded,
# IOPLoadModule("...") returns < 0 and the *_Init() call is skipped,
# so the boot proceeds (memory-card save is unavailable on emulator,
# but the menu and the SNES pipeline can run).
# NETPLAY.IRX stays embedded - it's already gated on bLoadedNetwork
# and is only loaded when the IP stack came up (i.e. real PS2 with
# SMAP).
EMBED_IRX_NAMES := netplay audsrv freesd
EMBED_HEADERS := $(patsubst %,$(EMBED_DIR)/%_irx.h,$(EMBED_IRX_NAMES))

AUDSRV_IRX_PATH ?= $(PS2SDK)/iop/irx/audsrv.irx
FREESD_IRX_PATH ?= $(PS2SDK)/iop/irx/freesd.irx

.PHONY: all clean strip list count package package-irx check-env

all: check-env $(TARGET)

check-env:
	@test -d "$(PS2SDK)" || (echo "ERRO: PS2SDK nao encontrado em $(PS2SDK)"; exit 1)
	@test -d "$(IRX_DIR)" || (echo "ERRO: pasta de IRX nao encontrada em $(IRX_DIR)"; exit 1)

$(OBJ_DIR):
	@mkdir -p "$(OBJ_DIR)"

$(PKG_DIR):
	@mkdir -p "$(PKG_DIR)"

$(EMBED_DIR):
	@mkdir -p "$(EMBED_DIR)"

# bin2c emits a .c file containing both the array definition and the size
# value, with internal "#ifndef __<label>__" header guards. Renaming to .h
# lets us include each generated file exactly once into embedded_irx.cpp,
# which keeps the array definitions as ordinary file-scope globals.
$(EMBED_DIR)/netplay_irx.h: $(CUSTOM_IRX_DIR)/NETPLAY.IRX | $(EMBED_DIR)
	@echo "BIN2C $<"
	@$(BIN2C) "$<" "$@" netplay_irx
$(EMBED_DIR)/audsrv_irx.h: $(AUDSRV_IRX_PATH) | $(EMBED_DIR)
	@echo "BIN2C $<"
	@$(BIN2C) "$<" "$@" audsrv_irx
$(EMBED_DIR)/freesd_irx.h: $(FREESD_IRX_PATH) | $(EMBED_DIR)
	@echo "BIN2C $<"
	@$(BIN2C) "$<" "$@" freesd_irx
$(EMBED_DIR)/mcsave_irx.h: $(CUSTOM_IRX_DIR)/MCSAVE.IRX | $(EMBED_DIR)
	@echo "BIN2C $<"
	@$(BIN2C) "$<" "$@" mcsave_irx

# embedded_irx.cpp #includes the generated headers, so make sure they
# exist before that file is compiled.
$(OBJ_DIR)/platform/ps2/system/embedded_irx.o: $(EMBED_HEADERS)

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	@mkdir -p "$(dir $@)"
	@echo "CC  $<"
	@$(EE_CC) $(CFLAGS) $(INCS) -c $< -o $@

$(OBJ_DIR)/%.o: src/%.cpp | $(OBJ_DIR)
	@mkdir -p "$(dir $@)"
	@echo "CXX $<"
	@$(EE_CXX) $(CXXFLAGS) $(INCS) -c $< -o $@

$(OBJ_DIR)/%.o: src/%.s | $(OBJ_DIR)
	@mkdir -p "$(dir $@)"
	@echo "AS  $<"
	@$(EE_CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: src/%.S | $(OBJ_DIR)
	@mkdir -p "$(dir $@)"
	@echo "AS  $<"
	@$(EE_CC) $(CFLAGS) $(INCS) -c $< -o $@

$(TARGET): $(OBJS) | $(OBJ_DIR)
	@echo "LD  $@"
	@$(EE_CXX) -o $@ $(OBJS) $(LIBDIRS) $(LIBS)

strip: $(TARGET)
	@echo "STRIP $<"
	@$(EE_STRIP) "$(TARGET)"

package: check-env $(TARGET) package-irx

package-irx: | $(PKG_DIR)
	@set -e; \
	echo "PKG $(PKG_DIR)"; \
	cp "$(TARGET)" "$(PKG_DIR)/SNESticle.elf"; \
	copy_sdk() { \
		f="$$1"; found=""; \
		for cand in "$$f" "$$(printf '%s' "$$f" | tr '[:upper:]' '[:lower:]')" "$$(printf '%s' "$$f" | tr '[:lower:]' '[:upper:]')"; do \
			if [ -f "$(IRX_DIR)/$$cand" ]; then \
				cp "$(IRX_DIR)/$$cand" "$(PKG_DIR)/"; \
				echo "  + $$cand"; found=1; break; \
			fi; \
		done; \
		if [ -z "$$found" ]; then echo "  ! faltando $$f em $(IRX_DIR)"; fi; \
	}; \
	copy_custom() { \
		f="$$1"; \
		if [ -f "$(CUSTOM_IRX_DIR)/$$f" ]; then \
			cp "$(CUSTOM_IRX_DIR)/$$f" "$(PKG_DIR)/"; \
			echo "  + $$(basename "$(CUSTOM_IRX_DIR)")/$$f"; \
		else \
			echo "  ! faltando custom IRX $$f (procurei em $(CUSTOM_IRX_DIR))"; \
		fi; \
	}; \
	echo "== SDK network IRX =="; \
	for f in $(SDK_NET_IRX); do copy_sdk "$$f"; done; \
	echo "== SDK compat network IRX =="; \
	for f in $(SDK_COMPAT_NET_IRX); do copy_sdk "$$f"; done; \
	echo "== SDK memory card IRX =="; \
	for f in $(SDK_MC_IRX); do copy_sdk "$$f"; done; \
	echo "== SDK extra IRX =="; \
	for f in $(SDK_EXTRA_IRX); do copy_sdk "$$f"; done; \
	echo "== Project custom IRX =="; \
	for f in $(CUSTOM_IRX); do copy_custom "$$f"; done; \
	echo "Pronto: $(PKG_DIR)"
clean:
	rm -rf "$(OBJ_DIR)"

list:
	@printf '%s\n' $(SRCS)

count:
	@printf 'sources: %s\n' "$(words $(SRCS))"
	@printf 'objects: %s\n' "$(words $(OBJS))"


# ---- ISO (OPL-compatible, adapted from InfinityStation) ----
#
# OPL (Open PS2 Loader) exige que:
#   1. Nome do arquivo ISO: '<GAME_ID>.<NomeBonito>.iso'
#   2. ELF dentro da ISO chamado exatamente '<GAME_ID>' (sem extensao)
#   3. SYSTEM.CNF com 'BOOT2 = cdrom0:\<GAME_ID>;1'
#
# IMPORTANTE: NAO usar -iso-level 2 ou -full-iso9660-filenames!
# O CDVDMAN do OPL assume ISO9660 level 1 estrito com buffer de
# 14 caracteres por entrada do TOC. Nomes longos no PVD estouram
# o buffer e o OPL pinta a tela branca.
#
# -J -joliet-long adiciona um Joliet SVD com nomes originais (UCS-2)
# para que o launcher mostre nomes bonitos. O OPL so le o PVD
# (sector 16), entao a coexistencia e segura.
#
# SLUS_999.99 e um ID nao alocado pela Sony, comum em homebrew.
# Override: make iso ISO_GAME_ID=SLPM_625.99
#
# Uso:
#   make iso                          # gera ISO sem ROMs
#   make iso roms=<pasta>             # gera ISO com ROMs
#   make iso roms=<pasta> out=<pasta> # gera ISO + copia pra <pasta>

ISO_GAME_ID   ?= SLUS_999.99
ISO_GAME_NAME ?= SNESticle
ISO_LABEL     ?= SNESTICLE
ISO_ROOT_DIR  ?= $(OBJ_DIR)/iso_root
ISO_OUT       ?= $(OBJ_DIR)/$(ISO_GAME_ID).$(ISO_GAME_NAME).iso
ISO_BOOT      ?= $(ISO_GAME_ID)
ISO_VMODE     ?= NTSC

# User-facing knobs (lowercase)
out  ?=
roms ?=

.PHONY: iso-check iso-root iso

iso-check:
	@command -v xorriso >/dev/null 2>&1 \
	  || command -v genisoimage >/dev/null 2>&1 \
	  || command -v mkisofs >/dev/null 2>&1 \
	  || { echo "ERRO: nenhum gerador de ISO encontrado (xorriso, genisoimage ou mkisofs)."; \
	       echo "Instale com: apt install xorriso"; exit 1; }

iso-root: $(TARGET) iso-check
	@rm -rf "$(ISO_ROOT_DIR)"
	@mkdir -p "$(ISO_ROOT_DIR)"
	@cp "$(TARGET)" "$(ISO_ROOT_DIR)/$(ISO_BOOT)"
	@printf '%s\n' \
		"BOOT2 = cdrom0:\\$(ISO_BOOT);1" \
		"VER = 1.00" \
		"VMODE = $(ISO_VMODE)" > "$(ISO_ROOT_DIR)/SYSTEM.CNF"
	@echo "[iso-root] SYSTEM.CNF:"
	@cat "$(ISO_ROOT_DIR)/SYSTEM.CNF"
	@# Custom IRX modules. The ELF expects to find these next to itself
	@# on the disc - the IOP loader walks _MainLoop_BootDir ("cdrom0:\")
	@# first, so the custom CDVD/SJPCM2/MCSAVE/NETPLAY IRXs must ship
	@# alongside the ELF. Without them IOPLoadModule returns -203 and
	@# the corresponding subsystem (cdfs filesystem, audio, memcard
	@# saves, netplay) is silently disabled. Only NETPLAY.IRX is also
	@# embedded in the ELF; the others are loaded exclusively from disc.
	@for f in $(CUSTOM_IRX); do \
		if [ -f "$(CUSTOM_IRX_DIR)/$$f" ]; then \
			cp -f "$(CUSTOM_IRX_DIR)/$$f" "$(ISO_ROOT_DIR)/"; \
			echo "[iso-root] + $$f"; \
		else \
			echo "[iso-root] ! faltando custom IRX $$f em $(CUSTOM_IRX_DIR)"; \
		fi; \
	done
	@if [ -n "$(strip $(roms))" ]; then \
		if [ ! -d "$(roms)" ]; then \
			echo "ERRO: pasta de ROMs nao existe: $(roms)"; \
			exit 1; \
		fi; \
		mkdir -p "$(ISO_ROOT_DIR)/ROMS"; \
		find "$(roms)" -maxdepth 1 -type f \
			\( -iname '*.smc' -o -iname '*.sfc' -o -iname '*.swc' \
			   -o -iname '*.fig' -o -iname '*.zip' \) \
			-exec cp -f {} "$(ISO_ROOT_DIR)/ROMS/" \; ; \
		echo "[iso-root] ROMs copiadas de $(roms)"; \
	else \
		echo "[iso-root] Sem ROMs (use roms=<pasta> para incluir)"; \
	fi
	@if [ -d "$(CURDIR)/cdroot" ]; then \
		cp -a "$(CURDIR)/cdroot/." "$(ISO_ROOT_DIR)/"; \
		echo "[iso-root] cdroot extras copiados"; \
	fi

iso: iso-root
	@mkdir -p "$$(dirname "$(ISO_OUT)")"
	@if command -v xorriso >/dev/null 2>&1; then \
		xorriso -as mkisofs \
			-V "$(ISO_LABEL)" \
			-sysid PLAYSTATION \
			-A PLAYSTATION \
			-publisher PLAYSTATION \
			-J -joliet-long \
			-o "$(ISO_OUT)" \
			"$(ISO_ROOT_DIR)"; \
	elif command -v genisoimage >/dev/null 2>&1; then \
		genisoimage \
			-V "$(ISO_LABEL)" \
			-sysid PLAYSTATION \
			-A PLAYSTATION \
			-publisher PLAYSTATION \
			-J -joliet-long \
			-o "$(ISO_OUT)" \
			"$(ISO_ROOT_DIR)"; \
	elif command -v mkisofs >/dev/null 2>&1; then \
		mkisofs \
			-V "$(ISO_LABEL)" \
			-sysid PLAYSTATION \
			-A PLAYSTATION \
			-publisher PLAYSTATION \
			-J -joliet-long \
			-o "$(ISO_OUT)" \
			"$(ISO_ROOT_DIR)"; \
	fi
	@echo "[iso] $(ISO_OUT)"
	@if [ -n "$(strip $(out))" ]; then \
		mkdir -p "$(out)"; \
		cp -f "$(ISO_OUT)" "$(out)/"; \
		echo "[iso] copiada para $(out)/"; \
	fi
# ---- /ISO ----
