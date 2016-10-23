# Copyright 2011 The Android Open Source Project

ifneq ($(BUILD_TINY_ANDROID),true)

# 给出当前文件的路径
LOCAL_PATH := $(call my-dir)
# 这是一个脚本，用于清除之前的变量，避免影响
include $(CLEAR_VARS)

# 要编译的源代码列表
LOCAL_SRC_FILES := \
	charger.c \
	lights.c

ifeq ($(strip $(BOARD_CHARGER_DISABLE_INIT_BLANK)),true)
LOCAL_CFLAGS := -DCHARGER_DISABLE_INIT_BLANK
endif

ifeq ($(strip $(BOARD_CHARGER_ENABLE_SUSPEND)),true)
LOCAL_CFLAGS += -DCHARGER_ENABLE_SUSPEND
endif

# 模块名称，必须唯一，而且不能包含空格
LOCAL_MODULE := charger
LOCAL_MODULE_TAGS := optional
LOCAL_FORCE_STATIC_EXECUTABLE := true
# 编译结果的安装路径为根文件系统
# TARGET_ROOT_OUT:根文件系统
# TARGET_OUT:system文件系统
# TARGET_OUT_DATA:data文件系统
LOCAL_MODULE_PATH := $(TARGET_ROOT_OUT)
LOCAL_UNSTRIPPED_PATH := $(TARGET_ROOT_OUT_UNSTRIPPED)
# 上面两条命令的区别还不是很清楚

# 头文件的搜索路径，默认LOCAL_PATH
LOCAL_C_INCLUDES := bootable/recovery

# 编译时搜需要使用哪些动态库？以便在编译时进行链接
LOCAL_STATIC_LIBRARIES := libminui libpixelflinger_static libpng
ifeq ($(strip $(BOARD_CHARGER_ENABLE_SUSPEND)),true)
LOCAL_STATIC_LIBRARIES += libsuspend
endif
LOCAL_STATIC_LIBRARIES += libz libstdc++ libcutils liblog libm libc

# 最终得到可执行文件
include $(BUILD_EXECUTABLE)

# 这里好像开始编译了另外一个模块，应该是图片资源
define _add-charger-image
# 两个$$啥意思？
include $$(CLEAR_VARS)
LOCAL_MODULE := system_core_charger_$(notdir $(1))
LOCAL_MODULE_STEM := $(notdir $(1))
_img_modules += $$(LOCAL_MODULE)
LOCAL_SRC_FILES := $1
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $$(TARGET_ROOT_OUT)/res/images/charger
# 把他编译成一个独立的模块，提前编译成第三方库方便使用
# 该库将被拷贝到$PROJECT/obj/local和$PROJECT/libs/<abi>(stripped)
include $$(BUILD_PREBUILT)
endef

_img_modules :=
_images :=
$(foreach _img, $(call find-subdir-subdir-files, "images", "*.png"), \
  $(eval $(call _add-charger-image,$(_img))))

include $(CLEAR_VARS)
LOCAL_MODULE := charger_res_images
LOCAL_MODULE_TAGS := optional
LOCAL_REQUIRED_MODULES := $(_img_modules)
include $(BUILD_PHONY_PACKAGE)

_add-charger-image :=
_img_modules :=

endif
