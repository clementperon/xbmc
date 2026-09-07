/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#include "utils/GLBufferObject.h"

#include <atomic>
#include <cstdint>

// Draws CVideoBufferWebCodecs pictures: the VideoFrame the buffer names is
// imported into this renderer's texture on the browser main thread, in the
// render thread's own GL command order, and drawn with the GUI's RGBA shader.
// The browser does the colour conversion and scaling; the base class keeps
// geometry, aspect, zoom, rotation and the framebuffer path. See
// docs/wasm/ZERO_COPY.md §3.3.
class CRendererWebCodecs : public CLinuxRendererGLES
{
public:
  CRendererWebCodecs();
  ~CRendererWebCodecs() override;

  static CBaseRenderer* Create(CVideoBuffer* buffer);
  static bool Register();

  void AddVideoPicture(const VideoPicture& picture, int index) override;
  void RenderUpdate(
      int index, int index2, bool clear, unsigned int flags, unsigned int alpha) override;
  CRenderInfo GetRenderInfo() override;
  bool Supports(ERENDERFEATURE feature) const override;
  bool Supports(ESCALINGMETHOD method) const override;

protected:
  bool LoadShadersHook() override;
  bool RenderHook(int index) override;
  bool CreateTexture(int index) override;
  void DeleteTexture(int index) override;
  bool UploadTexture(int index) override;

private:
  GLenum TextureFilter() const;
  void ApplyTextureFilter();
  void UploadPendingFrames();

  static_assert(NUM_BUFFERS <= 32, "m_pendingUploads holds one bit per render buffer");
  // Bits set by AddVideoPicture on the VideoPlayer thread, taken by the render thread.
  std::atomic<uint32_t> m_pendingUploads{0};
  ESCALINGMETHOD m_appliedScalingMethod{VS_SCALINGMETHOD_MAX};
  KODI::UTILS::GL::CGLBufferObject m_posVBO{GL_ARRAY_BUFFER};
  KODI::UTILS::GL::CGLBufferObject m_texVBO{GL_ARRAY_BUFFER};
  KODI::UTILS::GL::CGLBufferObject m_ibo{GL_ELEMENT_ARRAY_BUFFER};
};
