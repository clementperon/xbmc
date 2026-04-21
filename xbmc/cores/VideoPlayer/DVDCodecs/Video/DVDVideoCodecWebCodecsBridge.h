/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

// Shared ABI between the C++ video codec and the Emscripten JS library that
// drives WebCodecs. All functions declared here are implemented in
// webcodecs_bridge.js and linked via --js-library.
//
// The JS library automatically proxies every call to the main browser thread
// (WebCodecs objects are DOM-thread only) via Emscripten's __proxy: 'sync'
// attribute, so callers may invoke these functions from any pthread.

#include <cstdint>

#ifdef __cplusplus
extern "C"
{
#endif

// Pixel format ids, shared with JS (must stay in sync with webcodecs_bridge.js).
enum WebCodecsPixelFormat
{
  WEBCODECS_PIXFMT_UNKNOWN = 0,
  WEBCODECS_PIXFMT_YUV420P = 1,
  WEBCODECS_PIXFMT_NV12 = 2,
};

// Status codes returned by webcodecs_push_packet.
enum WebCodecsPushStatus
{
  WEBCODECS_PUSH_QUEUED = 1,
  WEBCODECS_PUSH_EMPTY = 0,
  WEBCODECS_PUSH_HANDLE_NOT_FOUND = -1,
  WEBCODECS_PUSH_DECODER_FAILED = -2,
  WEBCODECS_PUSH_NOT_CONFIGURED = -3,
  WEBCODECS_PUSH_DECODE_THREW = -4,
  WEBCODECS_PUSH_BUSY = -5,
};

// Metadata written by webcodecs_copy_next_frame. Field offsets are locked
// down by static_assert below and mirrored as byte offsets in JS.
struct WebCodecsFrameInfo
{
  int32_t pixelFormat;
  int32_t width;
  int32_t height;
  int32_t yStride;
  int32_t uStride;
  int32_t vStride;
  int32_t uOffset;
  int32_t vOffset;
  int32_t keyFrame;
  int32_t payloadSize;
  double ptsSeconds;
  double durationSeconds;
};

#ifdef __cplusplus
} // extern "C"

#include <cstddef>

static_assert(sizeof(WebCodecsFrameInfo) == 56, "WebCodecsFrameInfo must be 56 bytes");
static_assert(offsetof(WebCodecsFrameInfo, pixelFormat) == 0, "pixelFormat offset");
static_assert(offsetof(WebCodecsFrameInfo, width) == 4, "width offset");
static_assert(offsetof(WebCodecsFrameInfo, height) == 8, "height offset");
static_assert(offsetof(WebCodecsFrameInfo, yStride) == 12, "yStride offset");
static_assert(offsetof(WebCodecsFrameInfo, uStride) == 16, "uStride offset");
static_assert(offsetof(WebCodecsFrameInfo, vStride) == 20, "vStride offset");
static_assert(offsetof(WebCodecsFrameInfo, uOffset) == 24, "uOffset offset");
static_assert(offsetof(WebCodecsFrameInfo, vOffset) == 28, "vOffset offset");
static_assert(offsetof(WebCodecsFrameInfo, keyFrame) == 32, "keyFrame offset");
static_assert(offsetof(WebCodecsFrameInfo, payloadSize) == 36, "payloadSize offset");
static_assert(offsetof(WebCodecsFrameInfo, ptsSeconds) == 40, "ptsSeconds offset");
static_assert(offsetof(WebCodecsFrameInfo, durationSeconds) == 48, "durationSeconds offset");

extern "C"
{
#endif

// Returns a positive decoder handle on success, 0 on failure.
int webcodecs_create_decoder(const char* codec,
                             int codedWidth,
                             int codedHeight,
                             const uint8_t* description,
                             int descriptionSize,
                             int annexB);

// Closes and unregisters the decoder. Handle becomes invalid.
void webcodecs_destroy_decoder(int handle);

// reset() drops pending work and requires reconfigure(); the bridge handles both.
int webcodecs_reset_decoder(int handle);

// Feeds one encoded packet. Returns a WebCodecsPushStatus.
int webcodecs_push_packet(int handle,
                          const uint8_t* data,
                          int size,
                          int keyFrame,
                          double ptsSeconds,
                          double durationSeconds);

// Reports how many frames are still pending in the output queue.
// (We deliberately do NOT call VideoDecoder.flush(): flush forces a new
//  keyframe, which breaks pause/resume where DVD_CODEC_CTRL_DRAIN is raised
//  transiently, not only at real EOF.)
int webcodecs_drain_decoder(int handle);

// Copies the next queued frame into [dst, dst+dstSize) and fills *info.
// Returns 1 on success, 0 if no frame available / too small buffer, -1 on
// decoder failure.
int webcodecs_copy_next_frame(int handle,
                              uint8_t* dst,
                              int dstSize,
                              struct WebCodecsFrameInfo* info);

// Reads accumulated queue stats.
int webcodecs_read_stats(int handle, int* droppedFrames, int* highWaterMark);

// Writes a NUL-terminated UTF-8 diagnostic string ("<state>|<error>") of at
// most (dstSize-1) bytes into dst. Returns the number of bytes written
// (excluding the NUL). A return of 0 means "nothing to report".
int webcodecs_take_error(int handle, char* dst, int dstSize);

#ifdef __cplusplus
}
#endif
