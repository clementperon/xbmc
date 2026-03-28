/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "WinEventsWasm.h"

#include "ServiceBroker.h"
#include "application/AppInboundProtocol.h"
#include "guilib/GUIWindowManager.h"
#include "input/keyboard/XBMC_keyboard.h"
#include "utils/log.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

namespace
{
#ifdef __EMSCRIPTEN__
CWinEventsWasm* g_events = nullptr;

EM_BOOL OnKeyDown(int /*eventType*/, const EmscriptenKeyboardEvent* e, void* /*userData*/)
{
  if (!g_events)
    return EM_FALSE;
  XBMC_Event ev{};
  ev.type = XBMC_KEYDOWN;
  ev.key.keysym.scancode = 0;
  ev.key.keysym.sym = static_cast<XBMCKey>(e->keyCode);
  ev.key.keysym.mod = XBMCKMOD_NONE;
  ev.key.keysym.unicode = 0;
  g_events->MessagePush(ev);
  return EM_TRUE;
}

EM_BOOL OnKeyUp(int /*eventType*/, const EmscriptenKeyboardEvent* e, void* /*userData*/)
{
  if (!g_events)
    return EM_FALSE;
  XBMC_Event ev{};
  ev.type = XBMC_KEYUP;
  ev.key.keysym.scancode = 0;
  ev.key.keysym.sym = static_cast<XBMCKey>(e->keyCode);
  ev.key.keysym.mod = XBMCKMOD_NONE;
  ev.key.keysym.unicode = 0;
  g_events->MessagePush(ev);
  return EM_TRUE;
}

EM_BOOL OnMouseMove(int /*eventType*/, const EmscriptenMouseEvent* e, void* /*userData*/)
{
  if (!g_events)
    return EM_FALSE;
  XBMC_Event ev{};
  ev.type = XBMC_MOUSEMOTION;
  ev.motion.x = static_cast<int16_t>(e->canvasX);
  ev.motion.y = static_cast<int16_t>(e->canvasY);
  g_events->MessagePush(ev);
  return EM_FALSE;
}

EM_BOOL OnMouseButtonDown(int /*eventType*/, const EmscriptenMouseEvent* e, void* /*userData*/)
{
  if (!g_events)
    return EM_FALSE;
  XBMC_Event ev{};
  ev.type = XBMC_MOUSEBUTTONDOWN;
  ev.button.x = static_cast<int16_t>(e->canvasX);
  ev.button.y = static_cast<int16_t>(e->canvasY);
  ev.button.button = static_cast<unsigned char>(e->button + 1);
  g_events->MessagePush(ev);
  return EM_TRUE;
}

EM_BOOL OnMouseButtonUp(int /*eventType*/, const EmscriptenMouseEvent* e, void* /*userData*/)
{
  if (!g_events)
    return EM_FALSE;
  XBMC_Event ev{};
  ev.type = XBMC_MOUSEBUTTONUP;
  ev.button.x = static_cast<int16_t>(e->canvasX);
  ev.button.y = static_cast<int16_t>(e->canvasY);
  ev.button.button = static_cast<unsigned char>(e->button + 1);
  g_events->MessagePush(ev);
  return EM_TRUE;
}

EM_BOOL OnResize(int /*eventType*/, const EmscriptenUiEvent* e, void* /*userData*/)
{
  if (!g_events)
    return EM_FALSE;
  XBMC_Event ev{};
  ev.type = XBMC_VIDEORESIZE;
  ev.resize.width = e->windowInnerWidth;
  ev.resize.height = e->windowInnerHeight;
  ev.resize.scale = 1.0;
  g_events->MessagePush(ev);
  return EM_FALSE;
}
#endif
} // namespace

CWinEventsWasm::CWinEventsWasm()
{
#ifdef __EMSCRIPTEN__
  g_events = this;
  emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, OnKeyDown);
  emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, OnKeyUp);
  emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, OnMouseMove);
  emscripten_set_mousedown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, OnMouseButtonDown);
  emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, OnMouseButtonUp);
  emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, EM_TRUE, OnResize);
#endif
}

CWinEventsWasm::~CWinEventsWasm()
{
#ifdef __EMSCRIPTEN__
  g_events = nullptr;
#endif
}

void CWinEventsWasm::MessagePush(const XBMC_Event& newEvent)
{
  std::unique_lock lock(m_mutex);
  m_events.push_back(newEvent);
}

bool CWinEventsWasm::MessagePump()
{
  bool ret = false;
  for (;;)
  {
    XBMC_Event pumpEvent{};
    {
      std::unique_lock lock(m_mutex);
      if (m_events.empty())
        break;
      pumpEvent = m_events.front();
      m_events.pop_front();
    }
    std::shared_ptr<CAppInboundProtocol> appPort = CServiceBroker::GetAppPort();
    if (appPort)
      ret |= appPort->OnEvent(pumpEvent);
  }
  return ret;
}
