#include "x3_log.h"
#include <cstdio>

namespace x3 {

void log(LogLevel level, std::string_view msg) {
    const char* tag = "[INFO]";
    switch (level) {
        case LogLevel::Info:  tag = "[INFO] "; break;
        case LogLevel::Warn:  tag = "[WARN] "; break;
        case LogLevel::Error: tag = "[ERROR]"; break;
    }
    std::FILE* out = (level == LogLevel::Error) ? stderr : stdout;
    std::fprintf(out, "%s %.*s\n", tag, static_cast<int>(msg.size()), msg.data());
    std::fflush(out); // flush so output survives redirection + forced exit
}

} // namespace x3
