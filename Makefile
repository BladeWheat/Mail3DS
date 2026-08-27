#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
#---------------------------------------------------------------------------------
TARGET		:=	Mail3DS
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
ROMFS		:=	romfs

# 应用信息（用于smdh图标）
APP_TITLE		:= Mail3DS
APP_DESCRIPTION	:= 3DS 电子邮件客户端
APP_AUTHOR		:= Wheat

#---------------------------------------------------------------------------------
# CIA 打包固定参数（make cia）
#---------------------------------------------------------------------------------
CIA_TITLE_ID	:= 0004000003A17000
CIA_PROC_NAME	:= MAIL3DS
CIA_PROD_CODE	:= CTR-P-MAIL
CIA_VERSION		:= 1040				# 1.1.0 (主*1024 + 次*16 + 修订)
CIA_SHORT		:= Mail3DS
CIA_LONG		:= 3DS 电子邮件客户端
CIA_AUTHOR		:= Wheat
CIA_BUILD		:= cia_build

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-fomit-frame-pointer -ffunction-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# 链接库顺序：citro2d → citro3d → curl → mbedtls → z → ctru → m
LIBS	:= -lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -liconv -lctru -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(CTRULIB)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES)) \
			$(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o)

export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES	:=	$(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h) \
			$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

# romfs资源目录（字体、拼音字典等）和smdh图标
export _3DSXFLAGS += --smdh=$(OUTPUT).smdh --romfs=$(CURDIR)/$(ROMFS)
ROMFS_FILES := $(shell find $(CURDIR)/$(ROMFS) -type f 2>/dev/null)

.PHONY: $(BUILD) clean all cia

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
# 一键打包 CIA：先编译，再走 banner→smdh→cxi→cia 四步
# 前置：bannertool / 3dsxtool / cxitool / makerom 已在 PATH
#       $(CIA_BUILD)/ 下需备好 banner_256x128.png(256x128横幅)、
#       silent.wav(静音启动音)、patch_smdh_title.ps1(中文长标题补丁)
#---------------------------------------------------------------------------------
cia: $(BUILD)
	@echo packaging CIA ...
	@mkdir -p $(CIA_BUILD)
	@bannertool makebanner -i $(CIA_BUILD)/banner_256x128.png -a $(CIA_BUILD)/silent.wav -o $(CIA_BUILD)/banner.bnr
	@bannertool makesmdh -s "$(CIA_SHORT)" -l "3DS Email Client" -p "$(CIA_AUTHOR)" -i icon.png -o $(CIA_BUILD)/mail3ds.smdh -r regionfree
	@powershell -ExecutionPolicy Bypass -File $(CIA_BUILD)/patch_smdh_title.ps1 "$(CIA_BUILD)/mail3ds.smdh" "$(CIA_LONG)"
	@3dsxtool $(TARGET).elf $(CIA_BUILD)/Mail3DS.3dsx --smdh=$(CIA_BUILD)/mail3ds.smdh --romfs=$(ROMFS)
	@cxitool -s $(CIA_BUILD)/cxi_settings.yaml -t $(CIA_TITLE_ID) -n $(CIA_PROC_NAME) -c $(CIA_PROD_CODE) -b $(CIA_BUILD)/banner.bnr $(CIA_BUILD)/Mail3DS.3dsx $(CIA_BUILD)/Mail3DS.cxi
	@makerom -f cia -o $(CIA_BUILD)/Mail3DS.cia -target t -ver $(CIA_VERSION) -ignoresign -content $(CIA_BUILD)/Mail3DS.cxi:0:0
	@echo "==> CIA 打包完成: $(CIA_BUILD)/Mail3DS.cia"

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).elf $(TARGET).smdh romfs.bin

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(OUTPUT).smdh $(ROMFS_FILES)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	:	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
-include $(DEPSDIR)/*.d
#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------