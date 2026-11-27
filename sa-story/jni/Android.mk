LOCAL_PATH := $(call my-dir)/..
ROOT := $(LOCAL_PATH)/..

include $(CLEAR_VARS)
LOCAL_MODULE := SAStory
LOCAL_CPP_EXTENSION := .cpp
LOCAL_SRC_FILES := src/main.cpp src/log.cpp src/game.cpp src/script.cpp src/story.cpp src/engine.cpp src/missions.cpp \
                   src/ui.cpp src/imgui_rw.cpp \
                   ../third_party/imgui/imgui.cpp ../third_party/imgui/imgui_draw.cpp \
                   ../third_party/imgui/imgui_widgets.cpp ../third_party/imgui/imgui_tables.cpp \
                   ../third_party/mod/logger.cpp ../third_party/mod/config.cpp
LOCAL_C_INCLUDES += $(ROOT)/third_party $(LOCAL_PATH)/src
LOCAL_CFLAGS += -O2 -DNDEBUG -Wall -Wno-unused-function -fvisibility=hidden -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
LOCAL_CPPFLAGS += -std=c++17
LOCAL_LDLIBS += -llog
include $(BUILD_SHARED_LIBRARY)
