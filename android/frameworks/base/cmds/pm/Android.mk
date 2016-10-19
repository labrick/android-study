# Copyright 2007 The Android Open Source Project
#
# 这里是编译出pm.jar？？，adb shell中执行的pm都是这个jar文件？？
# 这是第一步
LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES := $(call all-subdir-java-files)
LOCAL_MODULE := pm
include $(BUILD_JAVA_LIBRARY)


include $(CLEAR_VARS)
ALL_PREBUILT += $(TARGET_OUT)/bin/pm
$(TARGET_OUT)/bin/pm : $(LOCAL_PATH)/pm | $(ACP)
	$(transform-prebuilt-to-target)

