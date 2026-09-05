/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

// Shared ABI between the C++ video codec and the Emscripten JS library that
// drives WebCodecs. All functions declared here are implemented in
// webcodecs_bridge.js and linked via --js-library.
//
// The VideoDecoder lives on the browser main thread, so the JS library proxies
// every call there with Emscripten's __proxy attribute; callers may invoke
// these functions from any pthread. The two per-packet calls are asynchronous
// and report through WebCodecsSharedState, the others are synchronous.

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
  // Packed 32-bit RGB, one plane; the codec converts these to YUV420P.
  WEBCODECS_PIXFMT_RGBA = 3,
  WEBCODECS_PIXFMT_RGBX = 4,
  WEBCODECS_PIXFMT_BGRA = 5,
  WEBCODECS_PIXFMT_BGRX = 6,
};

// Outcome of a webcodecs_copy_next_frame request, published in
// WebCodecsSharedState::copyResult once copyDone carries the request's id.
enum WebCodecsCopyResult
{
  WEBCODECS_COPY_OK = 1,
  WEBCODECS_COPY_FAILED = -1,
  WEBCODECS_COPY_DST_TOO_SMALL = -2,
  WEBCODECS_COPY_NO_FRAME = -3,
};

// Cap on decoder frames alive at once: pushed but not yet run by the main
// thread, queued for decode, awaiting copy, or being copied. Hardware decoders
// stall when too many output frames stay open.
enum
{
  WEBCODECS_MAX_INFLIGHT = 12
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

// Decoder state mirrored into wasm memory by the JS side with Atomics so the
// codec can poll it without a main-thread round trip. `signal` is incremented
// and Atomics.notify'd on every change, so callers can futex-wait on it.
struct WebCodecsSharedState
{
  int32_t signal;
  int32_t queuedFrames;
  int32_t inflight; // decodeQueueSize + frame copies still in progress
  int32_t failed;
  int32_t nextPayloadSize; // of the frame webcodecs_copy_next_frame returns next
  int32_t nextPixelFormat;
  int32_t pushesProcessed; // webcodecs_push_packet calls the main thread has run
  int32_t copyDone; // copyId of the last finished webcodecs_copy_next_frame
  int32_t copyResult; // WebCodecsCopyResult of that copy
};

#ifdef __cplusplus
} // extern "C"

#include <cstddef>

static_assert(sizeof(WebCodecsSharedState) == 36, "WebCodecsSharedState must be 36 bytes");
static_assert(offsetof(WebCodecsSharedState, signal) == 0, "signal offset");
static_assert(offsetof(WebCodecsSharedState, queuedFrames) == 4, "queuedFrames offset");
static_assert(offsetof(WebCodecsSharedState, inflight) == 8, "inflight offset");
static_assert(offsetof(WebCodecsSharedState, failed) == 12, "failed offset");
static_assert(offsetof(WebCodecsSharedState, nextPayloadSize) == 16, "nextPayloadSize offset");
static_assert(offsetof(WebCodecsSharedState, nextPixelFormat) == 20, "nextPixelFormat offset");
static_assert(offsetof(WebCodecsSharedState, pushesProcessed) == 24, "pushesProcessed offset");
static_assert(offsetof(WebCodecsSharedState, copyDone) == 28, "copyDone offset");
static_assert(offsetof(WebCodecsSharedState, copyResult) == 32, "copyResult offset");

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

// Returns a positive decoder handle on success, 0 on failure. `description` is
// the AVC or HEVC decoder configuration record when the stream has one, and
// `annexB` selects the Annex B bitstream format for those two codecs. `shared`
// must stay valid until webcodecs_destroy_decoder.
int webcodecs_create_decoder(const char* codec,
                             int codedWidth,
                             int codedHeight,
                             const uint8_t* description,
                             int descriptionSize,
                             int annexB,
                             struct WebCodecsSharedState* shared);

// Closes and unregisters the decoder. Handle becomes invalid.
void webcodecs_destroy_decoder(int handle);

// reset() drops pending work and requires reconfigure(); the bridge handles both.
int webcodecs_reset_decoder(int handle);

// Feeds one encoded packet. Runs asynchronously on the main thread and takes
// ownership of `data`, which must come from malloc(). Each call is counted in
// WebCodecsSharedState::pushesProcessed once it has run; a packet the decoder
// rejects puts it into the failed state.
void webcodecs_push_packet(int handle,
                           uint8_t* data,
                           int size,
                           int keyFrame,
                           double ptsSeconds,
                           double durationSeconds);

// Copies the next queued frame into [dst, dst+dstSize) and fills *info. Runs
// asynchronously on the main thread; when the copy has finished,
// WebCodecsSharedState::copyDone is set to copyId and copyResult to a
// WebCodecsCopyResult. dst and info must stay valid until then.
void webcodecs_copy_next_frame(int handle,
                               int copyId,
                               uint8_t* dst,
                               int dstSize,
                               struct WebCodecsFrameInfo* info);

// Fills *info with the next queued frame's metadata and closes the frame without
// copying it. Returns 1 if a frame was discarded, 0 if none was queued.
int webcodecs_discard_next_frame(int handle, struct WebCodecsFrameInfo* info);

// Reads accumulated queue stats.
int webcodecs_read_stats(int handle, int* droppedFrames, int* highWaterMark);

// Writes a NUL-terminated UTF-8 diagnostic string ("<state>|<error>") of at
// most (dstSize-1) bytes into dst. Returns the number of bytes written
// (excluding the NUL). A return of 0 means "nothing to report".
int webcodecs_take_error(int handle, char* dst, int dstSize);

#ifdef __cplusplus
}
#endif
