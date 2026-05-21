#pragma once
// Minimal logging for the engine. No third-party deps.
#include <string_view>

namespace x3 {

enum class LogLevel { Info, Warn, Error };

void log(LogLevel level, std::string_view msg);

inline void logInfo(std::string_view m)  { log(LogLevel::Info,  m); }
inline void logWarn(std::string_view m)  { log(LogLevel::Warn,  m); }
inline void logError(std::string_view m) { log(LogLevel::Error, m); }

} // namespace x3
