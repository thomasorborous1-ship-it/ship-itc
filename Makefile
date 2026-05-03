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

LEGACY_ROOT ?= $(CURDIR)/vendor/legacy
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

CFLAGS := -G0 -O2 -Wall \
	-D_EE -DPS2 -DLSB_FIRST -DALIGN_DWORD -DCODE_PLATFORM=3 \
	-DDEBUG_BOOT_SCREEN=$(DEBUG_BOOT_SCREEN) \
	-DMAINLOOP_DEBUG_GS_TEST=$(MAINLOOP_DEBUG_GS_TEST)

CXXFLAGS := -G0 -O2 -Wall -Wno-narrowing -Wno-overflow -fno-exceptions -fno-rtti -fpermissive \
	-D_EE -DPS2 -DLSB_FIRST -DALIGN_DWORD -DCODE_PLATFORM=3 \
	-DDEBUG_BOOT_SCREEN=$(DEBUG_BOOT_SCREEN) \
	-DMAINLOOP_DEBUG_GS_TEST=$(MAINLOOP_DEBUG_GS_TEST)

INCS := \
	-I$(EMBED_DIR) \
	-I$(CURDIR)/src/common/media \
	-I$(CURDIR)/src/platform/ps2/cdvd \
	-I$(CURDIR)/src/platform/ps2/gs \
	-I$(CURDIR)/src/platform/ps2/lowlevel \
	-I$(CURDIR)/src/platform/ps2 \
	-I$(CURDIR)/src/platform/ps2/ui \
	-I$(CURDIR)/src/platform/ps2/system \
	-I$(CURDIR)/src/platform/ps2/input \
	-I$(CURDIR)/src/platform/ps2/memcard \
	-I$(CURDIR)/src \
	-I$(LEGACY_ROOT)/Gep/Include/common \
	-I$(LEGACY_ROOT)/Gep/Include/ps2 \
	-I$(LEGACY_ROOT)/Gep/Source/common \
	-I$(LEGACY_ROOT)/Gep/Source/common/zlib \
	-I$(LEGACY_ROOT)/Gep/Include/common/zlib \
	-I$(LEGACY_ROOT)/Gep/Source/common/unzip \
	-I$(LEGACY_ROOT)/Gep/Include/common/unzip \
	-I$(LEGACY_ROOT)/SNESticle/Source/common \
	-I$(LEGACY_ROOT)/SNESticle/Source/ps2 \
	-I$(LEGACY_ROOT)/SNESticle/XML \
	-I$(LEGACY_ROOT)/SNESticle/Modules/mcsave/ee \
	-I$(LEGACY_ROOT)/SNESticle/Modules/sjpcm/ee \
	-I$(LEGACY_ROOT)/SNESticle/Modules/netplay/Source/common \
	-I$(LEGACY_ROOT)/SNESticle/Modules/netplay/Source/ps2/common \
	-I$(LEGACY_ROOT)/SNESticle/Modules/netplay/Source/ps2/ee \
	-I$(LEGACY_ROOT)/SNESticle/Modules/libcdvd/common \
	-I$(LEGACY_ROOT)/SNESticle/Modules/libcdvd/ee \
	-I$(CURDIR)/compat \
	-I$(PS2SDK)/common/include \
	-I$(PS2SDK)/ee/include \
	-I$(PS2SDK)/ports/include

LIBDIRS := \
	-L$(PS2SDK)/ee/lib \
	-L$(PS2SDK)/ports/lib

LIBS := \
	-lmc -lpad -lps2ip \
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
CUSTOM_IRX := CDVD.IRX NETPLAY.IRX MCSAVE.IRX SJPCM2.IRX

# IRX modules embedded directly into the ELF via bin2c. The custom
# IRX search paths (host:, cdrom:) used by the original code do not
# work on emulators or stripped-down PS2 setups, so we ship these
# modules inside the executable and load them via SifExecModuleBuffer.
#
# CDVD.IRX, SJPCM2.IRX and MCSAVE.IRX are intentionally NOT embedded:
# on NetherSX2 (and likely any IOP that isn't a 100% faithful real PS2)
# the iaddis custom IRXs do load via SifExecModuleBuffer, but their
# RPC entry points either never come up or are incompatible with the
# rom-resident services. The result is the EE-side init function
# (CDVD_Init, SjPCM_Init, MCSave_Init) spinning forever in SifBindRpc
# and deadlocking the boot. With these IRXs not embedded,
# IOPLoadModule("...") returns < 0 and the *_Init() call is skipped,
# so the boot proceeds (audio + memory-card save are unavailable on
# emulator, but the menu and the SNES CPU/PPU pipeline can run).
# NETPLAY.IRX stays embedded - it's already gated on bLoadedNetwork
# and is only loaded when the IP stack came up (i.e. real PS2 with
# SMAP).
EMBED_IRX_NAMES := netplay
EMBED_HEADERS := $(patsubst %,$(EMBED_DIR)/%_irx.h,$(EMBED_IRX_NAMES))

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
$(EMBED_DIR)/sjpcm2_irx.h: $(CUSTOM_IRX_DIR)/SJPCM2.IRX | $(EMBED_DIR)
	@echo "BIN2C $<"
	@$(BIN2C) "$<" "$@" sjpcm2_irx
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


# ---------------- ISO (CD/DVD) ----------------
ISO_LABEL ?= SNESTICLE
ISO_OUT   ?= $(OBJ_DIR)/$(ISO_LABEL).iso
ISO_DIR   ?= $(OBJ_DIR)/iso_root
BOOT_ELF  ?= $(ISO_LABEL).ELF
VMODE     ?= NTSC
VER       ?= 1.00

# tenta achar uma ferramenta tipo mkisofs
MKISOFS ?= $(shell if command -v xorriso >/dev/null 2>&1; then echo "xorriso -as mkisofs"; \
	elif command -v genisoimage >/dev/null 2>&1; then echo "genisoimage"; \
	elif command -v mkisofs >/dev/null 2>&1; then echo "mkisofs"; \
	else echo "mkisofs"; fi)

MKISOFSFLAGS ?= -J -R -l -iso-level 2

.PHONY: iso iso_stage iso_image

iso: package iso_stage iso_image
	@echo "ISO pronta: $(ISO_OUT)"

iso_stage: | $(OBJ_DIR)
	@set -e; \
	rm -rf "$(ISO_DIR)"; \
	mkdir -p "$(ISO_DIR)"; \
	echo "[ISO] copiando arquivos do pkg..."; \
	cp -a "$(PKG_DIR)/." "$(ISO_DIR)/"; \
		if [ -d "$(CURDIR)/cdroot" ]; then \
			echo "[ISO] copiando cdroot extras..."; \
			cp -a "$(CURDIR)/cdroot/." "$(ISO_DIR)/"; \
		fi; \
	# renomeia o ELF de boot para MAIUSCULO (padrao de disco)
	if [ -f "$(ISO_DIR)/SNESticle.elf" ]; then \
		mv "$(ISO_DIR)/SNESticle.elf" "$(ISO_DIR)/$(BOOT_ELF)"; \
	elif [ -f "$(ISO_DIR)/SNESticle.ELF" ]; then \
		mv "$(ISO_DIR)/SNESticle.ELF" "$(ISO_DIR)/$(BOOT_ELF)"; \
	fi; \
	# cria SYSTEM.CNF (na raiz) - ordem BOOT2/VER/VMODE
	printf "BOOT2 = cdrom0:\\%s;1\r\nVER = %s\r\nVMODE = %s\r\n" "$(BOOT_ELF)" "$(VER)" "$(VMODE)" > "$(ISO_DIR)/SYSTEM.CNF"; \
	echo "[ISO] SYSTEM.CNF:"; \
	cat "$(ISO_DIR)/SYSTEM.CNF"; \
	echo "[ISO] ISO_DIR=$(ISO_DIR)"

iso_image:
	@set -e; \
	echo "[ISO] gerando $(ISO_OUT) com: $(MKISOFS) $(MKISOFSFLAGS)"; \
	$(MKISOFS) $(MKISOFSFLAGS) -V "$(ISO_LABEL)" -o "$(ISO_OUT)" "$(ISO_DIR)"; \
	# se existir ps2bootgen no sistema, roda (opcional)
	if command -v ps2bootgen >/dev/null 2>&1; then \
		echo "[ISO] ps2bootgen detectado: aplicando licenca (opcional)"; \
		ps2bootgen -dvd -japan "$(ISO_OUT)"; \
	else \
		echo "[ISO] ps2bootgen nao encontrado (ok para OPL/emulador)"; \
	fi
# -------------- /ISO -----------------
