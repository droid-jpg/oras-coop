.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR 		?= 	$(CURDIR)
include $(DEVKITARM)/3ds_rules

CTRPFLIB	?=	$(DEVKITPRO)/libctrpf

TARGET		:= 	oras-coop
PLGINFO 	:= 	CTRPluginFramework.plgInfo

BUILD		:= 	Build
INCLUDES	:= 	Includes
SOURCES 	:= 	Sources

ARCH		:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS		:=	$(ARCH) -Os -mword-relocations \
				-fomit-frame-pointer -ffunction-sections -fno-strict-aliasing -Wall

CFLAGS		+=	$(INCLUDE) -D__3DS__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS		:=	$(ARCH)
LDFLAGS		:= -T $(TOPDIR)/3gx.ld $(ARCH) -Os -Wl,--gc-sections,--strip-discarded,--strip-debug

LIBS		:= -lctrpf -lctru
LIBDIRS		:= 	$(CTRPFLIB) $(CTRULIB) $(PORTLIBS)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CPPFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

export LD 		:= 	$(CXX)
export OFILES	:=	$(CPPFILES:.cpp=.o)
export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I $(CURDIR)/$(dir) ) \
					$(foreach dir,$(LIBDIRS),-I $(dir)/include) \
					-I $(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L $(dir)/lib)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).3gx $(OUTPUT).elf

re: clean all

else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).3gx : $(OUTPUT).elf
$(OUTPUT).elf : $(OFILES)

.PRECIOUS: %.elf
%.3gx: %.elf
	@echo creating $(notdir $@)
	@3gxtool -s $(word 1, $^) $(TOPDIR)/$(PLGINFO) $@

-include $(DEPENDS)

endif
