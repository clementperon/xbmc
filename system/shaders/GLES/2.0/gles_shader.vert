/*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#version 100

attribute vec4 m_attrpos;
attribute vec4 m_attrcol;
attribute vec4 m_attrcord0;
attribute vec4 m_attrcord1;
attribute vec4 m_attrsnap;
varying vec4 m_cord0;
varying vec4 m_cord1;
varying lowp vec4 m_colour;
uniform mat4 m_proj;
uniform mat4 m_model;
uniform mat4 m_coord0Matrix;
uniform float m_depth;
uniform mat4 m_gui;
uniform float m_snap;

void main ()
{
  vec4 pos = m_attrpos;
  if (m_snap > 0.0)
  {
    // Skin coordinates: apply the GUI transform and round to whole pixels. m_attrsnap holds the
    // opposite corner of the quad, and z = 1 pushes this corner one pixel away from it if both
    // round to the same row or column, so that thin quads never vanish.
    pos = m_gui * m_attrpos;
    pos.xyz = floor(pos.xyz + 0.5);
    vec2 opposite = floor((m_gui * vec4(m_attrsnap.xy, 0.0, 1.0)).xy + 0.5);
    pos.xy += vec2(equal(pos.xy, opposite)) * m_attrsnap.z;
  }

  mat4 mvp = m_proj * m_model;
  gl_Position = mvp * pos;
  gl_Position.z = m_depth * gl_Position.w;
  m_colour = m_attrcol;
  m_cord0 = m_coord0Matrix * m_attrcord0;
  m_cord1 = m_attrcord1;
}
