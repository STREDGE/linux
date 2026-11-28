LOCAL_PATH := $(call my-dir)/..
ROOT := $(LOCAL_PATH)/..

include $(CLEAR_VARS)
LOCAL_MODULE := SALiving
LOCAL_CPP_EXTENSION := .cpp
LOCAL_SRC_FILES := src/main.cpp src/settings.cpp src/util/json.cpp src/ai/claude.cpp src/game/game.cpp \
                   src/world/world.cpp src/talk/talk.cpp src/talk/prompt.cpp src/sim/police.cpp src/sim/life.cpp src/sim/crew.cpp \
                   src/net/link.cpp src/speech/speech.cpp src/speech/xvibe.cpp src/jarvis/jarvis.cpp src/jarvis/script.cpp src/jarvis/brain.cpp src/trace/trace.cpp src/ui/hud.cpp src/ui/imgui_rw.cpp src/platform/android.cpp src/platform/bridge.cpp src/squad/squad.cpp \
                   ../third_party/swat/sgame.cpp ../third_party/swat/catalog.cpp ../third_party/swat/team.cpp ../third_party/swat/driver.cpp \
                   ../third_party/imgui/imgui.cpp ../third_party/imgui/imgui_draw.cpp \
                   ../third_party/imgui/imgui_widgets.cpp ../third_party/imgui/imgui_tables.cpp \
                   ../third_party/mod/logger.cpp ../third_party/mod/config.cpp ../third_party/mod/flogger.cpp
LOCAL_C_INCLUDES += $(ROOT)/third_party $(LOCAL_PATH)/src
LOCAL_CFLAGS += -O2 -DNDEBUG -Wall -Wno-unused-function -fvisibility=hidden -DIMGUI_DISABLE_DEMO_WINDOWS -DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
LOCAL_CPPFLAGS += -std=c++17
LOCAL_LDLIBS += -llog -lz
include $(BUILD_SHARED_LIBRARY)
