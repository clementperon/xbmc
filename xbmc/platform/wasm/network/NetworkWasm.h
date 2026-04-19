/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "network/Network.h"

#include <string>
#include <vector>

class CNetworkWasm : public CNetworkBase
{
public:
  CNetworkWasm() = default;
  ~CNetworkWasm() override = default;

  std::vector<CNetworkInterface*>& GetInterfaceList() override;
  bool PingHost(unsigned long host, unsigned int timeout_ms = 2000) override;
  std::vector<std::string> GetNameServers() override;

private:
  std::vector<CNetworkInterface*> m_interfaces;
};
