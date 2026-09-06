/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#include "utils/GLBufferObject.h"

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

  ESCALINGMETHOD m_appliedScalingMethod{VS_SCALINGMETHOD_MAX};
  KODI::UTILS::GL::CGLBufferObject m_posVBO{GL_ARRAY_BUFFER};
  KODI::UTILS::GL::CGLBufferObject m_texVBO{GL_ARRAY_BUFFER};
  KODI::UTILS::GL::CGLBufferObject m_ibo{GL_ELEMENT_ARRAY_BUFFER};
};
