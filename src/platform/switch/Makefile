# =================================================================================
# Makefile — Nintendo Switch cross-compilation build for zelda3
#
# This Makefile uses the devkitPro ARM toolchain (devkitA64) to cross-compile the
# zelda3 C reimplementation for the Nintendo Switch. It produces either a homebrew
# .nro file (for the Homebrew Menu) or a .nsp sysmodule (if a JSON config is provided).
#
# The build targets an ARMv8-A Cortex-A57 (the Switch's Tegra X1 CPU) and links
# against libnx (Nintendo Switch homebrew library) and SDL2 for input/rendering.
#
# Source files are pulled from ../../ (the main src/ directory), including the SNES
# emulator core, OpenGL loader, and Opus audio decoder.
#
# Build requirements:
#   - devkitPro with devkitA64 and libnx installed
#   - DEVKITPRO environment variable pointing to the devkitPro installation
#   - SDL2 Switch port (available via devkitPro pacman)
# =================================================================================

#---------------------------------------------------------------------------------
# Clear all built-in suffix rules to prevent implicit pattern matching from
# interfering with the explicit rules defined below
.SUFFIXES:
#---------------------------------------------------------------------------------

# Guard: abort with a helpful message if the devkitPro toolchain path is not set
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

# TOPDIR defaults to the directory containing this Makefile
TOPDIR ?= $(CURDIR)
# Import libnx build rules — provides NRO/NSP/NACP recipes, bin2o, and toolchain vars
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# ROMFS is the directory containing data to be added to RomFS, relative to the Makefile (Optional)
#
# NO_ICON: if set to anything, do not use icon.
# NO_NACP: if set to anything, no .nacp file is generated.
# APP_TITLE is the name of the app stored in the .nacp file (Optional)
# APP_AUTHOR is the author of the app stored in the .nacp file (Optional)
# APP_VERSION is the version of the app stored in the .nacp file (Optional)
# APP_TITLEID is the titleID of the app stored in the .nacp file (Optional)
# ICON is the filename of the icon (.jpg), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.jpg
#     - icon.jpg
#     - <libnx folder>/default_icon.jpg
#
# CONFIG_JSON is the filename of the NPDM config file (.json), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.json
#     - config.json
#   If a JSON file is provided or autodetected, an ExeFS PFS0 (.nsp) is built instead
#   of a homebrew executable (.nro). This is intended to be used for sysmodules.
#   NACP building is skipped as well.
#---------------------------------------------------------------------------------
# Source directory is two levels up because this Makefile lives in src/platform/switch/
SRC_DIR  := $(abspath $(CURDIR)/../..)
ROOT_DIR := $(abspath $(CURDIR)/../../..)
TARGET		:=	zelda3
# BUILD directory for intermediate .o and .d files — matches the /bin entry in .gitignore
BUILD		:=	bin
# All source directories that contain compilable code: main game, SNES emulator,
# OpenGL 3.1 core loader, and the stripped Opus audio decoder for MSU-1 support
SOURCES		:=  $(SRC_DIR) $(SRC_DIR)/snes $(ROOT_DIR)/third_party/gl_core $(ROOT_DIR)/third_party/opus-1.3.1-stripped

# Explicit file list rather than directory wildcard — ensures only the intended .c files
# are compiled, and third-party amalgam files are included exactly once
CFILES := \
	$(wildcard $(SRC_DIR)/*.c) \
	$(wildcard $(SRC_DIR)/../snes/*.c) \
	$(ROOT_DIR)/third_party/gl_core/gl_core_3_1.c \
	$(ROOT_DIR)/third_party/opus-1.3.1-stripped/opus_decoder_amalgam.c
export CFILES

# Header search path — the local include/ dir holds any Switch-specific headers
INCLUDES := \
	$(SRC_DIR) \
	$(SRC_DIR)/.. \
	$(SRC_DIR)/../snes \
	$(ROOT_DIR)/third_party/gl_core \
	$(ROOT_DIR)/third_party/opus-1.3.1-stripped
# NACP metadata embedded into the .nro — displayed by the Switch Homebrew Menu
APP_TITLE   :=  The Legend of Zelda: A Link to the Past
APP_AUTHOR  :=  snesrev & Lywx
# Version string uses git short hash + branch name for traceability in homebrew builds
APP_VERSION :=  $(shell git rev-parse --short HEAD) $(shell git rev-parse --abbrev-ref HEAD)

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
# Target the Switch's Tegra X1 Cortex-A57 cores with ARMv8-A ISA extensions:
#   crc    — hardware CRC32 instructions
#   crypto — AES/SHA hardware acceleration
# -mtp=soft uses software thread pointer (libnx does not use the hardware TP register)
# -fPIE is required because Switch homebrew is loaded at arbitrary addresses
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# -g: include debug symbols for crash analysis on Switch
# -O2: optimize for speed (the Switch's A57 benefits significantly from inlining)
# -ffunction-sections: place each function in its own ELF section so the linker can
#   discard unused functions, reducing the final .nro size
# -Wno-parentheses: suppress warnings in the original zelda3 code that uses
#   assignments inside conditionals (a common C pattern in this codebase)
CFLAGS := -g -Wall -O2 -ffunction-sections -Wno-parentheses \
	-I$(SRC_DIR) \
	-I$(SRC_DIR)/.. \
	-I$(SRC_DIR)/../snes \
	-I$(ROOT_DIR)/third_party/gl_core \
	-I$(ROOT_DIR)/third_party/opus-1.3.1-stripped \
    $(ARCH) $(DEFINES)

# __SWITCH__ enables platform-specific code paths in the zelda3 source
# STBI_NO_THREAD_LOCALS disables TLS in stb_image since libnx TLS support is limited
# sdl2-config provides the correct include paths for the Switch SDL2 port
CFLAGS += -D__SWITCH__ $(INCLUDE) -DSTBI_NO_THREAD_LOCALS -DSDL_MAIN_HANDLED -include SDL.h `sdl2-config --cflags`

# C++ flags inherit C flags but disable RTTI and exceptions (not used, saves binary size)
CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions
# Force GNU C11 standard for C files — needed for anonymous structs/unions in zelda3 code
CFLAGS += -std=gnu11

# Assembly flags: include debug symbols and target the same ARM architecture as C code
ASFLAGS	:=	-g $(ARCH)
# switch.specs defines the linker script, entry point, and memory layout for Switch .elf
# The map file is generated alongside the build for symbol-level debugging
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# Link against SDL2 (input/video/audio), libnx (Switch OS services), and libm (math)
LIBS :=  `$(PREFIX)pkg-config --libs sdl2` -lnx -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
# PORTLIBS contains Switch ports of common libraries (SDL2, etc.)
# LIBNX contains the core Nintendo Switch homebrew library
LIBDIRS	:= $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
# ---------------------------------------------------------------------------------
# Two-phase build structure: when invoked from the source directory, this outer
# section sets up paths and re-invokes make from within the BUILD directory.
# When running inside BUILD, the inner section (after 'else') compiles objects.
# ---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

# Absolute path to the output binary — used by libnx rules to generate .nro/.nsp
export OUTPUT	:=	$(CURDIR)/$(TARGET)
# Re-export TOPDIR as an absolute path for use in the inner (BUILD-directory) phase
export TOPDIR	:=	$(CURDIR)

# VPATH tells make where to find source and data files when building from the
# BUILD subdirectory — allows .o targets to find their .c prerequisites
export VPATH := $(SRC_DIR) \
		$(SRC_DIR)/../snes \
		$(ROOT_DIR)/third_party/gl_core \
		$(ROOT_DIR)/third_party/opus-1.3.1-stripped

# Dependency (.d) files are generated alongside .o files in the BUILD directory
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

# These default wildcard expansions are commented out because CFILES is explicitly
# defined above to control exactly which source files are compiled
#CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
#CFILES    :=  $(wildcard ../../*.c ../../snes/*.c) ../../third_party/gl_core/gl_core_3_1.c ../../third_party/opus-1.3.1-stripped/opus_decoder_amalgam.c
#CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
#SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
# Binary data files (e.g., raw assets) to be converted to .o via bin2o
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
# Use the C++ linker even though the project is pure C — this ensures the C++
# standard library is available if any devkitPro library requires it
export LD	:=	$(CXX)
#---------------------------------------------------------------------------------

# Object files derived from binary data (converted via bin2o to linkable .o)
export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
# Object files derived from source code — strips directory paths via notdir so all
# .o files land flat in the BUILD directory regardless of source tree depth
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(notdir $(CFILES:.c=.o)) $(SFILES:.s=.o)
export OFILES 		:=	$(OFILES_BIN) $(OFILES_SRC)
# Auto-generated headers for binary data — allows C code to reference embedded data
# via extern symbols (e.g., a .bin file becomes a _bin.h with size/pointer declarations)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

# Build the -I include path list from: local includes, library includes, and the
# BUILD dir (where auto-generated headers from bin2o are placed)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(dir))

# Library search paths for the linker (-L flags)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

# Debug: print the list of object files during configuration
$(info $(OFILES))

# JSON config auto-detection: if CONFIG_JSON is not explicitly set, search for
# zelda3.json or config.json. Presence of a JSON config switches the build from
# producing a homebrew .nro to a sysmodule .nsp (with NPDM metadata).
ifeq ($(strip $(CONFIG_JSON)),)
	jsons := $(wildcard *.json)
	ifneq (,$(findstring $(TARGET).json,$(jsons)))
		export APP_JSON := $(TOPDIR)/$(TARGET).json
	else
		ifneq (,$(findstring config.json,$(jsons)))
			export APP_JSON := $(TOPDIR)/config.json
		endif
	endif
else
	export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

# Icon auto-detection: searches for zelda3.jpg, then icon.jpg, then falls back
# to libnx's default icon. The icon is embedded in the .nro and shown by HBMenu.
ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

# Embed the icon into the NRO unless NO_ICON is set
ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

# Embed the NACP metadata (title, author, version) into the NRO unless NO_NACP is set
ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

# If a title ID is provided, embed it into the NACP — required for installed titles
ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

# If a RomFS directory is specified, embed it into the NRO so the app can read
# bundled files at runtime via romfs:/ paths (e.g., zelda3_assets.dat, zelda3.ini)
ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

# Declare phony targets so make does not confuse them with real files.
# $(BUILD) must be phony because the directory always exists after the first build,
# and we always want the inner make invocation to run to check for source changes.
.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
# Default target: build the NRO (or NSP if JSON config is present)
all: $(BUILD)

# Create the BUILD directory if needed, then re-invoke make from inside it.
# Running from BUILD ensures object files are isolated from source files.
$(BUILD):
	@echo $(CFILES) ...
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
# Clean target removes all build artifacts. The set of files to remove differs
# between homebrew (.nro) and sysmodule (.nsp) builds.
clean:
	@echo clean ...
ifeq ($(strip $(APP_JSON)),)
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
	@rm -fr $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf
endif


#---------------------------------------------------------------------------------
# Inner build phase: this section runs when make is invoked from inside the BUILD
# directory. It defines the actual compilation and linking rules.
else
.PHONY:	all

# Auto-generated dependency files — gcc -M output that tracks header dependencies
# so that changing a .h file triggers recompilation of affected .c files
DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
# Build target selection: without a JSON config, produce a homebrew NRO;
# with one, produce a sysmodule NSP (contains NSO executable + NPDM permissions)
ifeq ($(strip $(APP_JSON)),)

all	:	$(OUTPUT).nro

# NRO depends on the raw ELF and (optionally) NACP metadata
ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro	:	$(OUTPUT).elf
endif

else

all	:	$(OUTPUT).nsp

# NSP packages a Nintendo Shared Object (NSO, compressed ELF) + NPDM (permissions)
$(OUTPUT).nsp	:	$(OUTPUT).nso $(OUTPUT).npdm

$(OUTPUT).nso	:	$(OUTPUT).elf

endif

# Link all object files into the final ELF binary
$(OUTPUT).elf	:	$(OFILES)

# Source object files depend on binary-data headers so they are generated first
$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
# Rule for converting raw binary data files into linkable object files and
# corresponding C headers. The bin2o tool (from devkitPro) wraps the binary
# in an ELF section and generates extern declarations for the data pointer and size.
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

# Include auto-generated dependency files so header changes trigger recompilation
-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
