/*
 *  Copyright (C) 2026 Team Kodi
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

// #region agent log: temporary debug instrumentation, remove when done.
#pragma once

#include <cstdint>
#include <string>

namespace KODI::PLATFORM::WASM::DEBUGLOG
{

// Posts a log entry (NDJSON) to the host debug endpoint.
// Safe to call from any pthread (Web Workers have fetch).
// dataJson must be a valid JSON object string (e.g. "{\"k\":1}"), or "{}".
void Post(const char* location, const char* message, const std::string& dataJson);

// Returns Date.now() (ms since epoch). Useful for time-deltas measured from JS clock.
double NowMs();

// Installs a 100ms setInterval on the main browser thread that updates a
// shared timestamp; lets workers tell whether main thread is alive.
// Idempotent: safe to call multiple times.
void InstallMainHeartbeat();

// Returns Date.now() value of the most recent main-thread heartbeat tick.
// Returns 0 if the heartbeat was never installed.
double MainHeartbeatLastMs();

} // namespace KODI::PLATFORM::WASM::DEBUGLOG
// #endregion
