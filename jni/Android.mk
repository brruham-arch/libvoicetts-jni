LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE        := voicetts
LOCAL_SRC_FILES     := voicetts.cpp
LOCAL_LDLIBS        := -llog -ldl
LOCAL_CPPFLAGS      := -std=c++17 -fvisibility=hidden -O2
LOCAL_CPP_FEATURES  := exceptions
include $(BUILD_SHARED_LIBRARY)
