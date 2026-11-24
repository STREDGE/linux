LOCAL_PATH := $(call my-dir)/..

include $(CLEAR_VARS)
LOCAL_MODULE := SACoop
LOCAL_CPP_EXTENSION := .cpp
LOCAL_SRC_FILES := src/main.cpp src/net/session.cpp src/game/gta.cpp src/coop/coop.cpp \
                   third_party/mod/logger.cpp \
                   third_party/mod/config.cpp \
                   third_party/mod/flogger.cpp
LOCAL_C_INCLUDES += $(LOCAL_PATH)/third_party $(LOCAL_PATH)/src
LOCAL_CFLAGS += -O2 -DNDEBUG -std=c++17 -Wall
LOCAL_LDLIBS += -llog
include $(BUILD_SHARED_LIBRARY)
