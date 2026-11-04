# PS4 homebrew build of Pokemon Silver (OpenOrbis PS4 Toolchain v0.5.2).
#
# Needs:
#   OO_PS4_TOOLCHAIN   path to the extracted OpenOrbis toolchain
#   GAMBATTE_DIR       checkout of libretro/gambatte-libretro   (default: ./gambatte)
#   assets/rom/pokesilver.gbc   ROM built by `make pokesilver.gbc` at the repo root
#   clang, clang++, ld.lld on PATH (or override CC/CXX/LD)

TITLE       := Silver
VERSION     := 1.00
TITLE_ID    := PKSV00001
CONTENT_ID  := IV0000-PKSV00001_00-POKESILVER000000

TOOLCHAIN    := $(OO_PS4_TOOLCHAIN)
GAMBATTE_DIR ?= gambatte
TOOLS        := $(TOOLCHAIN)/bin/linux
CC           ?= clang
CXX          := clang++
LD           := ld.lld
INTDIR       := build

GB           := $(GAMBATTE_DIR)/libgambatte
GB_SOURCES   := $(wildcard $(GB)/src/*.cpp $(GB)/src/mem/*.cpp $(GB)/src/sound/*.cpp $(GB)/src/video/*.cpp)
APP_SOURCES  := src/main.cpp
OBJS         := $(patsubst $(GB)/src/%.cpp,$(INTDIR)/gambatte/%.o,$(GB_SOURCES)) \
                $(patsubst src/%.cpp,$(INTDIR)/%.o,$(APP_SOURCES))

LIBS         := -lc -lkernel -lc++ -lSceUserService -lSceVideoOut -lSceAudioOut -lScePad -lSceSysmodule -lSDL2

DEFINES      := -D__LIBRETRO__ -DHAVE_STDINT_H -DHAVE_INTTYPES_H
INCLUDES     := -I$(GB)/include -I$(GB)/src -I$(GB)/libretro -I$(GB)/libretro-common/include -I$(GAMBATTE_DIR)/common
CXXFLAGS     := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -w \
                -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -isystem $(TOOLCHAIN)/include/c++/v1 \
                $(DEFINES) $(INCLUDES)
LDFLAGS      := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

ASSETS       := $(wildcard assets/*/*)
LIBMODULES   := $(wildcard sce_module/*)

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(TOOLS)/PkgTool.Core pkg_build $< .

pkg.gp4: eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_sys/pic1.png $(LIBMODULES) $(ASSETS)
	$(TOOLS)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"

sce_sys/param.sfo: Makefile
	$(TOOLS)/PkgTool.Core sfo_new $@
	$(TOOLS)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLS)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(TOOLS)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(TOOLS)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(TOOLS)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLS)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(TOOLS)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(TOOLS)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLS)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLS)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

eboot.bin: $(OBJS)
	$(LD) $(OBJS) -o $(INTDIR)/pokesilver.elf $(LDFLAGS)
	$(TOOLS)/create-fself -in=$(INTDIR)/pokesilver.elf -out=$(INTDIR)/pokesilver.oelf --eboot "eboot.bin" --paid 0x3800000000000011

$(INTDIR)/gambatte/%.o: $(GB)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(INTDIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(INTDIR) eboot.bin pkg.gp4 sce_sys/param.sfo $(CONTENT_ID).pkg

.PHONY: all clean
