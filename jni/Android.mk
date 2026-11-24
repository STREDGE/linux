LOCAL_PATH := $(call my-dir)/..

include $(CLEAR_VARS)
LOCAL_MODULE := SACoop
LOCAL_CPP_EXTENSION := .cpp
LOCAL_SRC_FILES := src/main.cpp src/settings.cpp src/net/session.cpp src/game/gta.cpp src/coop/coop.cpp \
                   src/ui/menu.cpp src/ui/overlay.cpp src/ui/imgui_rw.cpp src/platform/android.cpp \
                   third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp \
                   third_party/imgui/imgui_widgets.cpp third_party/imgui/imgui_tables.cpp \
                   third_party/mod/logger.cpp \
                   third_party/mod/config.cpp \
                   third_party/mod/flogger.cpp
LOCAL_C_INCLUDES += $(LOCAL_PATH)/third_party $(LOCAL_PATH)/src
LOCAL_CFLAGS += -O2 -DNDEBUG -std=c++17 -Wall -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
LOCAL_LDLIBS += -llog
include $(BUILD_SHARED_LIBRARY)
