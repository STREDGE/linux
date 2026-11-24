# Статическая сборка Opus (BSD, xiph.org) — кодек голосового чата. Чистый C, без NEON-вставок.
include $(CLEAR_VARS)
OPUS_DIR := $(LOCAL_PATH)/third_party/opus
include $(OPUS_DIR)/celt_sources.mk
include $(OPUS_DIR)/silk_sources.mk
include $(OPUS_DIR)/opus_sources.mk
LOCAL_MODULE := opus
LOCAL_SRC_FILES := $(addprefix third_party/opus/,$(CELT_SOURCES) $(SILK_SOURCES) $(SILK_SOURCES_FLOAT) $(OPUS_SOURCES) $(OPUS_SOURCES_FLOAT))
LOCAL_C_INCLUDES := $(OPUS_DIR)/include $(OPUS_DIR)/celt $(OPUS_DIR)/silk $(OPUS_DIR)/silk/float $(OPUS_DIR)/src
LOCAL_CFLAGS := -O2 -DOPUS_BUILD -DUSE_ALLOCA -DHAVE_LRINT -DHAVE_LRINTF -DNDEBUG -w
include $(BUILD_STATIC_LIBRARY)
