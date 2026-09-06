/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "TizenWebApis.h"

#include "utils/JSONVariantParser.h"
#include "utils/Variant.h"
#include "utils/log.h"

#include <cstdlib>
#include <mutex>

#include <emscripten.h>
#include <emscripten/em_asm.h>

namespace KODI::PLATFORM::WASM
{
namespace
{

// Takes ownership of a stringToNewUTF8() result (0 when the JS side failed).
bool ParseOwnedJson(void* ptr, CVariant& out)
{
  if (!ptr)
    return false;

  const bool ok = CJSONVariantParser::Parse(static_cast<const char*>(ptr), out);
  std::free(ptr);
  return ok && out.isObject();
}

TizenNetworkInfo::ConnectionType ToConnectionType(int64_t samsungType)
{
  // NetworkActiveConnectionType values of the Samsung Network API.
  switch (samsungType)
  {
    case 0:
      return TizenNetworkInfo::ConnectionType::DISCONNECTED;
    case 1:
      return TizenNetworkInfo::ConnectionType::WIFI;
    case 2:
      return TizenNetworkInfo::ConnectionType::CELLULAR;
    case 3:
      return TizenNetworkInfo::ConnectionType::ETHERNET;
    default:
      return TizenNetworkInfo::ConnectionType::UNKNOWN;
  }
}

std::string ToIpMode(int64_t samsungMode)
{
  // NetworkIpMode values of the Samsung Network API.
  switch (samsungMode)
  {
    case 1:
      return "static";
    case 2:
      return "DHCP";
    case 3:
      return "auto";
    case 4:
      return "fixed";
    default:
      return {};
  }
}

} // namespace

TizenNetworkInfo CTizenWebApis::GetNetworkInfo()
{
  TizenNetworkInfo info;

  // Every getter may throw (SecurityError without the network.public privilege,
  // InvalidStateError while disconnected), so each one is read independently.
  // clang-format off
  void* json = MAIN_THREAD_EM_ASM_PTR(({
    try {
      if (typeof webapis === 'undefined' || !webapis.network)
        return 0;
      const net = webapis.network;
      const out = {};
      const read = (key, fn) => {
        try {
          out[key] = fn();
        } catch (e) {
          if (!(e && e.name === 'InvalidStateError'))
            console.warn('[kodi] webapis.network.' + key + ':', e);
        }
      };
      read('type', () => net.getActiveConnectionType());
      read('gatewayConnected', () => net.isConnectedToGateway());
      read('ipMode', () => net.getIpMode());
      read('ip', () => net.getIp());
      read('netmask', () => net.getSubnetMask());
      read('gateway', () => net.getGateway());
      read('mac', () => net.getMac());
      read('dns1', () => net.getDns());
      read('dns2', () => net.getSecondaryDns());
      read('tvName', () => net.getTVName());
      if (out.type === 1) {
        read('ssid', () => net.getWiFiSsid());
        read('signal', () => net.getWiFiSignalStrengthLevel());
        read('frequency', () => net.getWiFiFrequency());
      }
      return stringToNewUTF8(JSON.stringify(out));
    } catch (e) {
      console.warn('[kodi] webapis.network snapshot failed:', e);
      return 0;
    }
  }));
  // clang-format on

  CVariant data;
  if (!ParseOwnedJson(json, data))
    return info;

  info.available = true;
  info.connectionType = ToConnectionType(data["type"].asInteger(-1));
  info.gatewayConnected = data["gatewayConnected"].asBoolean();
  info.ipMode = ToIpMode(data["ipMode"].asInteger(-1));
  info.ipAddress = data["ip"].asString();
  info.netmask = data["netmask"].asString();
  info.gateway = data["gateway"].asString();
  info.macAddress = data["mac"].asString();
  info.dns1 = data["dns1"].asString();
  info.dns2 = data["dns2"].asString();
  info.tvName = data["tvName"].asString();
  info.wifiSsid = data["ssid"].asString();
  info.wifiSignalLevel = data["signal"].asInteger32(-1);
  info.wifiFrequencyMHz = data["frequency"].asInteger32(0);

  // Samsung reports "0.0.0.0" for addresses it does not have.
  for (std::string* addr : {&info.ipAddress, &info.netmask, &info.gateway, &info.dns1, &info.dns2})
  {
    if (*addr == "0.0.0.0")
      addr->clear();
  }

  return info;
}

TizenDeviceInfo CTizenWebApis::GetDeviceInfo()
{
  // Device details never change, but webapis.js may still be loading during
  // early startup, so only a successful snapshot is kept.
  static std::mutex cacheMutex;
  static TizenDeviceInfo cached;
  {
    std::lock_guard lock(cacheMutex);
    if (cached.available)
      return cached;
  }

  TizenDeviceInfo info;

  // clang-format off
  void* json = MAIN_THREAD_EM_ASM_PTR(({
    try {
      const hasProduct = typeof webapis !== 'undefined' && !!webapis.productinfo;
      const hasTizen = typeof tizen !== 'undefined' && !!tizen.systeminfo;
      if (!hasProduct && !hasTizen)
        return 0;
      const out = {};
      const read = (key, fn) => {
        try {
          out[key] = fn();
        } catch (e) {
          console.warn('[kodi] tizen device info ' + key + ':', e);
        }
      };
      if (hasProduct) {
        read('model', () => webapis.productinfo.getModel());
        read('realModel', () => webapis.productinfo.getRealModel());
        read('firmware', () => webapis.productinfo.getFirmware());
      }
      if (hasTizen) {
        read('platformVersion',
             () => tizen.systeminfo.getCapability('http://tizen.org/feature/platform.version'));
      }
      return stringToNewUTF8(JSON.stringify(out));
    } catch (e) {
      console.warn('[kodi] tizen device info snapshot failed:', e);
      return 0;
    }
  }));
  // clang-format on

  CVariant data;
  if (!ParseOwnedJson(json, data))
    return info;

  info.available = true;
  info.model = data["model"].asString();
  info.realModel = data["realModel"].asString();
  info.firmware = data["firmware"].asString();
  info.platformVersion = data["platformVersion"].asString();

  CLog::Log(LOGINFO, "Tizen device: model '{}' ({}), firmware '{}', platform {}", info.realModel,
            info.model, info.firmware, info.platformVersion);

  std::lock_guard lock(cacheMutex);
  cached = info;
  return info;
}

bool CTizenWebApis::SetScreenSaverEnabled(bool enabled)
{
  // clang-format off
  return MAIN_THREAD_EM_ASM_INT(({
    try {
      if (typeof webapis === 'undefined' || !webapis.appcommon)
        return 0;
      const states = webapis.appcommon.AppCommonScreenSaverState;
      webapis.appcommon.setScreenSaver(
          $0 ? states.SCREEN_SAVER_ON : states.SCREEN_SAVER_OFF, () => {},
          (e) => console.warn('[kodi] webapis.appcommon.setScreenSaver:', e));
      return 1;
    } catch (e) {
      console.warn('[kodi] webapis.appcommon.setScreenSaver failed:', e);
      return 0;
    }
  }), enabled ? 1 : 0) != 0;
  // clang-format on
}

} // namespace KODI::PLATFORM::WASM
