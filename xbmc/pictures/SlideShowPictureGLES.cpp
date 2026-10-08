/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "SlideShowPictureGLES.h"

#include "ServiceBroker.h"
#include "guilib/Texture.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "utils/GLUtils.h"
#include "windowing/WinSystem.h"

std::unique_ptr<CSlideShowPic> CSlideShowPic::CreateSlideShowPicture()
{
  return std::make_unique<CSlideShowPicGLES>();
}

void CSlideShowPicGLES::Free()
{
  m_posVBO.Destroy();
  m_texVBO.Destroy();
  m_IBO.Destroy();
}

void CSlideShowPicGLES::Render(float* x,
                               float* y,
                               CTexture* pTexture,
                               KODI::UTILS::COLOR::Color color)
{
  CRenderSystemGLES* renderSystem =
      dynamic_cast<CRenderSystemGLES*>(CServiceBroker::GetRenderSystem());
  if (pTexture)
  {
    pTexture->LoadToGPU();
    pTexture->BindToUnit(0);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND); // Turn Blending On

    renderSystem->EnableGUIShader(ShaderMethodGLES::SM_TEXTURE);
  }
  else
  {
    renderSystem->EnableGUIShader(ShaderMethodGLES::SM_DEFAULT);
  }

  float u1 = 0, u2 = 1, v1 = 0, v2 = 1;
  if (pTexture)
  {
    u2 = (float)pTexture->GetWidth() / pTexture->GetTextureWidth();
    v2 = (float)pTexture->GetHeight() / pTexture->GetTextureHeight();
  }

  GLubyte col[4];
  static constexpr GLfloat ver[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
  GLfloat tex[4][2];
  GLubyte idx[4] = {0, 1, 3, 2}; //determines order of triangle strip

  GLint posLoc = renderSystem->GUIShaderGetPos();
  GLint tex0Loc = renderSystem->GUIShaderGetCoord0();
  GLint uniColLoc = renderSystem->GUIShaderGetUniCol();
  GLint depthLoc = renderSystem->GUIShaderGetDepth();

  // Setup Colour values
  col[0] = KODI::UTILS::GL::GetChannelFromARGB(KODI::UTILS::GL::ColorChannel::R, color);
  col[1] = KODI::UTILS::GL::GetChannelFromARGB(KODI::UTILS::GL::ColorChannel::G, color);
  col[2] = KODI::UTILS::GL::GetChannelFromARGB(KODI::UTILS::GL::ColorChannel::B, color);
  col[3] = KODI::UTILS::GL::GetChannelFromARGB(KODI::UTILS::GL::ColorChannel::A, color);

  if (CServiceBroker::GetWinSystem()->UseLimitedColor())
  {
    col[0] = (235 - 16) * col[0] / 255 + 16;
    col[1] = (235 - 16) * col[1] / 255 + 16;
    col[2] = (235 - 16) * col[2] / 255 + 16;
  }

  // The corners are always a transformed rectangle, so they are the image of the unit quad.
  // clang-format off
  const GLfloat matrix[16] = {x[1] - x[0], y[1] - y[0], 0.0f, 0.0f,
                              x[3] - x[0], y[3] - y[0], 0.0f, 0.0f,
                              0.0f,        0.0f,        1.0f, 0.0f,
                              x[0],        y[0],        0.0f, 1.0f};
  // clang-format on
  glUniformMatrix4fv(renderSystem->GUIShaderGetGUIMatrix(), 1, GL_FALSE, matrix);

  // Setup texture coordinates
  tex[0][0] = tex[3][0] = u1;
  tex[0][1] = tex[1][1] = v1;
  tex[1][0] = tex[2][0] = u2;
  tex[2][1] = tex[3][1] = v2;

  m_posVBO.SetDataOnce(ver);
  glVertexAttribPointer(posLoc, 2, GL_FLOAT, 0, 0, 0);
  glEnableVertexAttribArray(posLoc);

  m_texVBO.SetDataIfChanged(tex);
  glVertexAttribPointer(tex0Loc, 2, GL_FLOAT, 0, 0, 0);
  glEnableVertexAttribArray(tex0Loc);

  m_IBO.SetDataOnce(idx);

  glUniform4f(uniColLoc, (col[0] / 255.0f), (col[1] / 255.0f), (col[2] / 255.0f),
              (col[3] / 255.0f));
  glUniform1f(depthLoc, -1.0f);
  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, 0);

  glDisableVertexAttribArray(posLoc);
  glDisableVertexAttribArray(tex0Loc);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

  renderSystem->DisableGUIShader();
}
