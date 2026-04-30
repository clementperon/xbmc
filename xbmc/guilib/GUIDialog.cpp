/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "GUIDialog.h"

#include "GUIComponent.h"
#include "GUIControlFactory.h"
#include "GUILabelControl.h"
#include "GUIWindowManager.h"
#include "ServiceBroker.h"
#include "input/actions/Action.h"
#include "messaging/ApplicationMessenger.h"
#include "threads/SingleLock.h"
#include "utils/TimeUtils.h"
#include "utils/XTimeUtils.h"
#include "windowing/WinSystem.h"

// #region agent log: temporary debug instrumentation, remove when done.
#if defined(TARGET_WASM)
#include "platform/wasm/DebugLog.h"
#include <chrono>
#include <cstdio>
#endif
// #endregion

CGUIDialog::CGUIDialog(int id, const std::string &xmlFile, DialogModalityType modalityType /* = DialogModalityType::MODAL */)
    : CGUIWindow(id, xmlFile)
{
  m_modalityType = modalityType;
  m_wasRunning = false;
  m_renderOrder = RENDER_ORDER_DIALOG;
  m_autoClosing = false;
  m_showStartTime = 0;
  m_showDuration = 0;
  m_enableSound = true;
  m_bAutoClosed = false;
}

CGUIDialog::~CGUIDialog(void) = default;

bool CGUIDialog::Load(TiXmlElement* pRootElement)
{
  return CGUIWindow::Load(pRootElement);
}

void CGUIDialog::OnWindowLoaded()
{
  CGUIWindow::OnWindowLoaded();

  // Clip labels to extents
  if (!m_children.empty())
  {
    CGUIControl* pBase = m_children[0];

    for (iControls p = m_children.begin() + 1; p != m_children.end(); ++p)
    {
      if ((*p)->GetControlType() == CGUIControl::GUICONTROL_LABEL)
      {
        CGUILabelControl* pLabel = (CGUILabelControl*)(*p);

        if (!pLabel->GetWidth())
        {
          float spacing = (pLabel->GetXPosition() - pBase->GetXPosition()) * 2;
          pLabel->SetWidth(pBase->GetWidth() - spacing);
        }
      }
    }
  }
}

bool CGUIDialog::OnAction(const CAction &action)
{
  // keyboard or controller movement should prevent autoclosing
  if (!action.IsMouse() && m_autoClosing)
    SetAutoClose(m_showDuration);

  return CGUIWindow::OnAction(action);
}

bool CGUIDialog::OnBack(int actionID)
{
  Close();
  return true;
}

bool CGUIDialog::OnMessage(CGUIMessage& message)
{
  switch ( message.GetMessage() )
  {
  case GUI_MSG_WINDOW_DEINIT:
    {
      CGUIWindow::OnMessage(message);
      return true;
    }
  case GUI_MSG_WINDOW_INIT:
    {
      CGUIWindow::OnMessage(message);
      m_showStartTime = 0;
      return true;
    }
  }

  return CGUIWindow::OnMessage(message);
}

void CGUIDialog::OnDeinitWindow(int nextWindowID)
{
  if (m_active)
  {
    CServiceBroker::GetGUI()->GetWindowManager().RemoveDialog(GetID());
    m_autoClosing = false;
  }
  CGUIWindow::OnDeinitWindow(nextWindowID);
}

void CGUIDialog::DoProcess(unsigned int currentTime, CDirtyRegionList &dirtyregions)
{
  UpdateVisibility();

  // if we were running but now we're not, mark us dirty
  if (!m_active && m_wasRunning)
    dirtyregions.emplace_back(m_renderRegion);

  if (m_active)
    CGUIWindow::DoProcess(currentTime, dirtyregions);

  m_wasRunning = m_active;
}

void CGUIDialog::UpdateVisibility()
{
  if (m_visibleCondition)
  {
    if (m_visibleCondition->Get(INFO::DEFAULT_CONTEXT))
      Open();
    else
      Close();
  }

  if (m_autoClosing)
  { // check if our timer is running
    if (!m_showStartTime)
    {
      if (HasProcessed()) // start timer
        m_showStartTime = CTimeUtils::GetFrameTime();
    }
    else
    {
      if (m_showStartTime + m_showDuration < CTimeUtils::GetFrameTime() && !m_closing)
      {
        m_bAutoClosed = true;
        Close();
      }
    }
  }
}

void CGUIDialog::Open_Internal(bool bProcessRenderLoop, const std::string &param /* = "" */)
{
  if (!CServiceBroker::GetGUI()->GetWindowManager().Initialized() ||
      (m_active && !m_closing && !IsAnimating(ANIM_TYPE_WINDOW_CLOSE)))
    return;

  // set running before it's added to the window manager, else the auto-show code
  // could show it as well if we are in a different thread from the main rendering
  // thread (this should really be handled via a thread message though IMO)
  m_active = true;
  m_closing = false;
  CServiceBroker::GetGUI()->GetWindowManager().RegisterDialog(this);

  // active this window
  CGUIMessage msg(GUI_MSG_WINDOW_INIT, 0, 0);
  msg.SetStringParam(param);
  OnMessage(msg);

  // process render loop
  if (bProcessRenderLoop)
  {
    if (!m_windowLoaded)
      Close(true);

// #region agent log: temporary debug instrumentation, remove when done.
#if defined(TARGET_WASM)
    using namespace KODI::PLATFORM::WASM::DEBUGLOG;
    InstallMainHeartbeat();
    const double loopStartMs = NowMs();
    Post("GUIDialog.cpp:Open_Internal:enter", "modal loop enter",
         std::string("{\"dialogId\":") + std::to_string(GetID()) + "}");
    uint64_t iterCount = 0;
    uint64_t eventfulIters = 0;
    uint64_t totalPumpUs = 0;
    uint64_t totalRenderUs = 0;
    uint64_t totalSleepUs = 0;
    uint64_t maxIterUs = 0;
    double lastReportMs = loopStartMs;
#endif
// #endregion
    while (m_active)
    {
// #region agent log: temporary debug instrumentation, remove when done.
#if defined(TARGET_WASM)
      const auto iterT0 = std::chrono::steady_clock::now();
#endif
// #endregion
      const bool processedEvents = PumpPlatformEvents();
// #region agent log: temporary debug instrumentation, remove when done.
#if defined(TARGET_WASM)
      const auto iterT1 = std::chrono::steady_clock::now();
#endif
// #endregion
      if (!CServiceBroker::GetGUI()->GetWindowManager().ProcessRenderLoop(false))
        break;
// #region agent log: temporary debug instrumentation, remove when done.
#if defined(TARGET_WASM)
      const auto iterT2 = std::chrono::steady_clock::now();
#endif
// #endregion
#if defined(TARGET_WASM)
      if (!processedEvents)
        KODI::TIME::Sleep(std::chrono::milliseconds(1));
#endif
// #region agent log: temporary debug instrumentation, remove when done.
#if defined(TARGET_WASM)
      const auto iterT3 = std::chrono::steady_clock::now();
      const auto pumpUs =
          std::chrono::duration_cast<std::chrono::microseconds>(iterT1 - iterT0).count();
      const auto renderUs =
          std::chrono::duration_cast<std::chrono::microseconds>(iterT2 - iterT1).count();
      const auto sleepUs =
          std::chrono::duration_cast<std::chrono::microseconds>(iterT3 - iterT2).count();
      const auto totalUs =
          std::chrono::duration_cast<std::chrono::microseconds>(iterT3 - iterT0).count();
      ++iterCount;
      if (processedEvents)
        ++eventfulIters;
      totalPumpUs += pumpUs;
      totalRenderUs += renderUs;
      totalSleepUs += sleepUs;
      if (static_cast<uint64_t>(totalUs) > maxIterUs)
        maxIterUs = totalUs;
      if (totalUs >= 500'000)
      {
        char buf2[256];
        std::snprintf(buf2, sizeof(buf2),
                      "{\"pumpUs\":%lld,\"renderUs\":%lld,\"sleepUs\":%lld,"
                      "\"totalUs\":%lld,\"nowMs\":%.0f}",
                      static_cast<long long>(pumpUs), static_cast<long long>(renderUs),
                      static_cast<long long>(sleepUs), static_cast<long long>(totalUs),
                      NowMs());
        Post("GUIDialog.cpp:Open_Internal:slowIter", "modal slow iter", buf2);
      }

      const double nowMs = NowMs();
      if (nowMs - lastReportMs >= 1000.0)
      {
        char buf[640];
        std::snprintf(buf, sizeof(buf),
                      "{\"dialogId\":%d,\"iters\":%llu,\"eventful\":%llu,\"avgPumpUs\":%llu,"
                      "\"avgRenderUs\":%llu,\"avgSleepUs\":%llu,\"maxIterUs\":%llu,"
                      "\"sinceStartMs\":%.0f,\"nowMs\":%.0f}",
                      GetID(), static_cast<unsigned long long>(iterCount),
                      static_cast<unsigned long long>(eventfulIters),
                      static_cast<unsigned long long>(iterCount ? totalPumpUs / iterCount : 0),
                      static_cast<unsigned long long>(iterCount ? totalRenderUs / iterCount : 0),
                      static_cast<unsigned long long>(iterCount ? totalSleepUs / iterCount : 0),
                      static_cast<unsigned long long>(maxIterUs), nowMs - loopStartMs, nowMs);
        Post("GUIDialog.cpp:Open_Internal:tick", "modal loop tick", buf);
        lastReportMs = nowMs;
        iterCount = 0;
        eventfulIters = 0;
        totalPumpUs = totalRenderUs = totalSleepUs = 0;
        maxIterUs = 0;
      }
#endif
// #endregion
    }
// #region agent log: temporary debug instrumentation, remove when done.
#if defined(TARGET_WASM)
    Post("GUIDialog.cpp:Open_Internal:exit", "modal loop exit",
         std::string("{\"dialogId\":") + std::to_string(GetID()) +
             ",\"durationMs\":" + std::to_string(NowMs() - loopStartMs) + "}");
#endif
// #endregion
  }
}

void CGUIDialog::Open(const std::string &param /* = "" */)
{
  CGUIDialog::Open(m_modalityType != DialogModalityType::MODELESS, param);
}


void CGUIDialog::Open(bool bProcessRenderLoop, const std::string& param /* = "" */)
{
  if (!CServiceBroker::GetAppMessenger()->IsProcessThread())
  {
    // make sure graphics lock is not held
    CSingleExit leaveIt(CServiceBroker::GetWinSystem()->GetGfxContext());
    CServiceBroker::GetAppMessenger()->SendMsg(TMSG_GUI_DIALOG_OPEN, -1, bProcessRenderLoop,
                                               static_cast<void*>(this), param);
  }
  else
    Open_Internal(bProcessRenderLoop, param);
}

void CGUIDialog::Render()
{
  if (!m_active)
    return;

  CGUIWindow::Render();
}

void CGUIDialog::SetDefaults()
{
  CGUIWindow::SetDefaults();
  m_renderOrder = RENDER_ORDER_DIALOG;
}

void CGUIDialog::SetAutoClose(unsigned int timeoutMs)
{
   m_autoClosing = true;
   m_showDuration = timeoutMs;
   ResetAutoClose();
}

void CGUIDialog::ResetAutoClose(void)
{
  if (m_autoClosing && m_active)
    m_showStartTime = CTimeUtils::GetFrameTime();
}

void CGUIDialog::CancelAutoClose(void)
{
  m_autoClosing = false;
}

void CGUIDialog::ProcessRenderLoop(bool renderOnly)
{
  PumpPlatformEvents();
  CServiceBroker::GetGUI()->GetWindowManager().ProcessRenderLoop(renderOnly);
}

bool CGUIDialog::PumpPlatformEvents()
{
#if defined(TARGET_WASM)
  // Modal dialogs run a nested render loop that bypasses the normal
  // CWinSystemBase::DriveRenderLoop() message pump. Keep the platform event
  // pump alive here so wasm browser/remote input continues to reach Kodi.
  return CServiceBroker::GetWinSystem()->MessagePump();
#endif
  return false;
}
