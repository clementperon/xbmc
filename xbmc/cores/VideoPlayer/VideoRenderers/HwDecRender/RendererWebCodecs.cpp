/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "RendererWebCodecs.h"

#include "../RenderFactory.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecWebCodecs.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "utils/GLUtils.h"
#include "utils/log.h"

namespace
{
constexpr unsigned int RENDER_BUFFERS = 4;
} // namespace

CRendererWebCodecs::CRendererWebCodecs()
{
  // Configure() waives its host-plane check for a custom renderer.
  m_renderMethod = RENDER_CUSTOM;
  m_textureTarget = GL_TEXTURE_2D;
}

CRendererWebCodecs::~CRendererWebCodecs()
{
  for (int i = 0; i < NUM_BUFFERS; ++i)
    DeleteTexture(i);
}

CBaseRenderer* CRendererWebCodecs::Create(CVideoBuffer* buffer)
{
  if (buffer && dynamic_cast<CVideoBufferWebCodecs*>(buffer))
    return new CRendererWebCodecs();
  return nullptr;
}

bool CRendererWebCodecs::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("webcodecs", CRendererWebCodecs::Create);
  return true;
}

CRenderInfo CRendererWebCodecs::GetRenderInfo()
{
  CRenderInfo info;
  info.max_buffer_size = RENDER_BUFFERS;
  return info;
}

bool CRendererWebCodecs::Supports(ERENDERFEATURE feature) const
{
  // The browser converts the frame to the canvas colour space before Kodi
  // sees it.
  if (feature == RENDERFEATURE_TONEMAP)
    return false;
  return CLinuxRendererGLES::Supports(feature);
}

bool CRendererWebCodecs::Supports(ESCALINGMETHOD method) const
{
  return method == VS_SCALINGMETHOD_NEAREST || method == VS_SCALINGMETHOD_LINEAR ||
         method == VS_SCALINGMETHOD_AUTO;
}

bool CRendererWebCodecs::LoadShadersHook()
{
  CLog::Log(LOGINFO, "GL: Using WebCodecs render method");
  m_textureTarget = GL_TEXTURE_2D;
  m_renderMethod = RENDER_CUSTOM;
  return true;
}

GLenum CRendererWebCodecs::TextureFilter() const
{
  return m_videoSettings.m_ScalingMethod == VS_SCALINGMETHOD_NEAREST ? GL_NEAREST : GL_LINEAR;
}

// UpdateVideoFilter() refilters RENDER_GLSL textures only, so a method changed
// during playback is applied here.
void CRendererWebCodecs::ApplyTextureFilter()
{
  const GLenum filter = TextureFilter();
  for (int i = 0; i < m_NumYUVBuffers; ++i)
  {
    const GLuint texture = m_buffers[i].fields[FIELD_FULL][0].id;
    if (!texture)
      continue;
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  m_appliedScalingMethod = m_videoSettings.m_ScalingMethod;
}

// One texture per buffer and no storage: the upload's texImage2D specifies
// level 0 at the frame's size each time.
bool CRendererWebCodecs::CreateTexture(int index)
{
  CPictureBuffer& buf = m_buffers[index];
  buf.image.width = m_sourceWidth;
  buf.image.height = m_sourceHeight;
  buf.image.cshift_x = 0;
  buf.image.cshift_y = 0;

  for (int field = 0; field < MAX_FIELDS; ++field)
  {
    CYuvPlane& plane = buf.fields[field][0];
    plane.texwidth = m_sourceWidth;
    plane.texheight = m_sourceHeight;
    plane.pixpertex_x = 1;
    plane.pixpertex_y = 1;
  }

  CYuvPlane& plane = buf.fields[FIELD_FULL][0];
  if (!plane.id)
    glGenTextures(1, &plane.id);

  const GLenum filter = TextureFilter();
  glBindTexture(GL_TEXTURE_2D, plane.id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
  m_appliedScalingMethod = m_videoSettings.m_ScalingMethod;
  return true;
}

void CRendererWebCodecs::DeleteTexture(int index)
{
  ReleaseBuffer(index);

  CPictureBuffer& buf = m_buffers[index];
  CYuvPlane& plane = buf.fields[FIELD_FULL][0];
  if (plane.id)
  {
    glDeleteTextures(1, &plane.id);
    plane.id = 0;
  }
  buf.loaded = false;
}

// Queues the import of the frame into this buffer's texture. The bridge closes
// the frame once it is in the texture, so a buffer is uploaded once; `loaded`
// keeps the texture for repeated and paused frames.
bool CRendererWebCodecs::UploadTexture(int index)
{
  CPictureBuffer& buf = m_buffers[index];
  if (buf.loaded)
    return true;

  auto* videoBuffer = dynamic_cast<CVideoBufferWebCodecs*>(buf.videoBuffer);
  const GLuint texture = buf.fields[FIELD_FULL][0].id;
  if (!videoBuffer || !texture)
    return false;

  webcodecs_upload_frame(videoBuffer->GetDecoderHandle(), videoBuffer->GetSequence(), texture);
  buf.loaded = true;
  return true;
}

bool CRendererWebCodecs::RenderHook(int index)
{
  if (m_appliedScalingMethod != m_videoSettings.m_ScalingMethod)
    ApplyTextureFilter();

  CalculateTextureSourceRects(index, 1);
  const CYuvPlane& plane = m_buffers[index].fields[FIELD_FULL][0];

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, plane.id);

  m_renderSystem->EnableGUIShader(ShaderMethodGLES::SM_TEXTURE_RGBA);
  glUniform1f(m_renderSystem->GUIShaderGetContrast(), m_videoSettings.m_Contrast * 0.02f);
  glUniform1f(m_renderSystem->GUIShaderGetBrightness(),
              m_videoSettings.m_Brightness * 0.01f - 0.5f);
  glUniform1f(m_renderSystem->GUIShaderGetDepth(), -1.0f);

  GLfloat vertices[4][4];
  GLfloat coords[4][4];
  for (int i = 0; i < 4; ++i)
  {
    vertices[i][0] = m_rotatedDestCoords[i].x;
    vertices[i][1] = m_rotatedDestCoords[i].y;
    vertices[i][2] = 0.0f;
    vertices[i][3] = 1.0f;
    coords[i][2] = 0.0f;
    coords[i][3] = 1.0f;
  }
  // Row 0 of the texture is the top of the frame, as with Kodi's own uploads.
  coords[0][0] = coords[3][0] = plane.rect.x1;
  coords[0][1] = coords[1][1] = plane.rect.y1;
  coords[1][0] = coords[2][0] = plane.rect.x2;
  coords[2][1] = coords[3][1] = plane.rect.y2;

  const GLint posLoc = m_renderSystem->GUIShaderGetPos();
  const GLint texLoc = m_renderSystem->GUIShaderGetCoord0();

  m_posVBO.SetData(vertices, GL_STREAM_DRAW);
  glVertexAttribPointer(posLoc, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(posLoc);

  m_texVBO.SetData(coords, GL_STREAM_DRAW);
  glVertexAttribPointer(texLoc, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(texLoc);

  const GLubyte indices[4] = {0, 1, 3, 2};
  m_ibo.SetDataOnce(indices);
  glDrawElements(GL_TRIANGLE_STRIP, 4, GL_UNSIGNED_BYTE, nullptr);

  glDisableVertexAttribArray(posLoc);
  glDisableVertexAttribArray(texLoc);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

  m_renderSystem->DisableGUIShader();
  glBindTexture(GL_TEXTURE_2D, 0);
  VerifyGLState();
  return true;
}
