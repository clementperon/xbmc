/*
 *  Copyright (C) 2020 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PosixInterfaceForCLog.h"

#include "ServiceBroker.h"
#include "application/AppParams.h"

#if defined(TARGET_WASM)
#include <emscripten.h>
#include <spdlog/sinks/base_sink.h>
#endif
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#if defined(TARGET_WASM)
namespace
{
class CWasmConsoleSink : public spdlog::sinks::base_sink<std::mutex>
{
protected:
  void sink_it_(const spdlog::details::log_msg& msg) override
  {
    spdlog::memory_buf_t formatted;
    this->formatter_->format(msg, formatted);
    std::string text(formatted.data(), formatted.size());

    int consoleMethod = 3;
    switch (msg.level)
    {
      case spdlog::level::trace:
      case spdlog::level::debug:
        consoleMethod = 0;
        break;
      case spdlog::level::info:
        consoleMethod = 1;
        break;
      case spdlog::level::warn:
        consoleMethod = 2;
        break;
      case spdlog::level::err:
      case spdlog::level::critical:
      case spdlog::level::off:
      default:
        consoleMethod = 3;
        break;
    }

    EM_ASM(
        {
          const text = UTF8ToString($0);
          switch ($1)
          {
            case 0:
              console.debug(text);
              break;
            case 1:
              console.info(text);
              break;
            case 2:
              console.warn(text);
              break;
            default:
              console.error(text);
              break;
          }
        },
        text.c_str(), consoleMethod);
  }

  void flush_() override {}
};
} // namespace
#endif

#if !defined(TARGET_ANDROID) && !defined(TARGET_DARWIN)
std::unique_ptr<IPlatformLog> IPlatformLog::CreatePlatformLog()
{
  return std::make_unique<CPosixInterfaceForCLog>();
}
#endif

void CPosixInterfaceForCLog::AddSinks(
    std::shared_ptr<spdlog::sinks::dist_sink<std::mutex>> distributionSink) const
{
  if (CServiceBroker::GetAppParams()->GetLogTarget() == "console")
#if defined(TARGET_WASM)
    distributionSink->add_sink(std::make_shared<CWasmConsoleSink>());
#else
    distributionSink->add_sink(std::make_shared<spdlog::sinks::stdout_color_sink_st>());
#endif
}
