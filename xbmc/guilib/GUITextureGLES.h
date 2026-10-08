/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "GUITexture.h"
#include "utils/ColorUtils.h"
#include "utils/GLBufferObject.h"

#include <array>
#include <cstddef>
#include <vector>

#include "system_gl.h"

struct PackedVertex
{
  float x, y, z;
  float u1, v1;
  float u2, v2;
};
typedef std::vector<PackedVertex> PackedVertices;

class CGUIQuadDrawerGLES;
class CRenderSystemGLES;

class CGUITextureGLES : public CGUITexture
{
public:
  static void Register(CGUIQuadDrawerGLES& quadDrawer);
  static CGUITexture* CreateTexture(
      float posX, float posY, float width, float height, const CTextureInfo& texture);

  CGUITextureGLES(float posX, float posY, float width, float height, const CTextureInfo& texture);
  ~CGUITextureGLES() override = default;

  CGUITextureGLES* Clone() const override;

protected:
  void Free() override;
  void Begin(KODI::UTILS::COLOR::Color color) override;
  void Draw(float* x, float* y, float* z, const CRect& texture, const CRect& diffuse, int orientation) override;
  void End() override;
  bool DrawQuads(const std::vector<Quad>& quads, unsigned int version) override;

private:
  CGUITextureGLES(const CGUITextureGLES& texture);

  struct QuadVertex
  {
    float x, y;
    float oppositeX, oppositeY, push; // see m_attrsnap in gles_shader.vert
    float u1, v1;
    float u2, v2;
  };

  std::array<GLubyte, 4> m_col;

  PackedVertices m_packedVertices;

  KODI::UTILS::GL::CGLBufferObject m_quadBuffer{GL_ARRAY_BUFFER};
  unsigned int m_quadBufferVersion{0};
  std::size_t m_quadCount{0};

  CRenderSystemGLES *m_renderSystem;
  bool m_isGLES20{true};
};

