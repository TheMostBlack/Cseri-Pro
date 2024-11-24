LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE  := zstd

LOCAL_SRC_FILES := lib/zstd/$(TARGET_ARCH_ABI)/libzstd.a

include $(PREBUILT_STATIC_LIBRARY)

include $(CLEAR_VARS)

LOCAL_C_INCLUDES := $(LOCAL_PATH)/../lua
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/zlib
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/snappy
LOCAL_C_INCLUDES += $(LOCAL_PATH)/include/zstd
LOCAL_MODULE     := cseri
LOCAL_CFLAGS := -std=c23 -O3 -ffast-math
LOCAL_CPPFLAGS := -std=c++23 -O3 -ffast-math
LOCAL_SRC_FILES  := \
    snappy/snappy-c.cc \
    snappy/snappy-sinksource.cc \
    snappy/snappy-stubs-internal.cc \
    snappy/snappy.cc \
    zlib/adler32.c \
    zlib/compress.c \
    zlib/crc32.c \
    zlib/deflate.c \
    zlib/gzclose.c \
    zlib/gzlib.c \
    zlib/gzread.c \
    zlib/gzwrite.c \
    zlib/infback.c \
    zlib/inffast.c \
    zlib/inflate.c \
    zlib/inftrees.c \
    zlib/trees.c \
    zlib/uncompr.c \
    zlib/zutil.c \
    binary.c \
    buffer.c \
    cseri.c \
    text.c

LOCAL_STATIC_LIBRARIES := luajava

LOCAL_STATIC_LIBRARIES += zstd

include $(BUILD_SHARED_LIBRARY)
