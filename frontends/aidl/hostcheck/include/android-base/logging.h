#pragma once
#include <ostream>
#include <sstream>
namespace android::base {
enum LogId { RADIO };
struct LogdLogger { explicit LogdLogger(LogId) {} };
inline void InitLogging(char **, LogdLogger) {}
struct LogLine { std::ostringstream s; template <typename T> LogLine &operator<<(const T &v) { s << v; return *this; } };
}  // namespace android::base
#define LOG(severity) ::android::base::LogLine()
#define PLOG(severity) ::android::base::LogLine()
#define CHECK(c) ::android::base::LogLine()
#define CHECK_EQ(a, b) ((void)((a) == (b)), ::android::base::LogLine())
