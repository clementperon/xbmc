/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "NetworkWasm.h"

std::unique_ptr<CNetworkBase> CNetworkBase::GetNetwork()
{
  return std::make_unique<CNetworkWasm>();
}

std::vector<CNetworkInterface*>& CNetworkWasm::GetInterfaceList()
{
  return m_interfaces;
}

bool CNetworkWasm::PingHost(unsigned long host, unsigned int timeout_ms)
{
  return false;
}

std::vector<std::string> CNetworkWasm::GetNameServers()
{
  return {};
}
