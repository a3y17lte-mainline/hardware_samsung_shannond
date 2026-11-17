#pragma once
#include <string>
namespace android::base {
inline std::string GetProperty(const std::string &, const std::string &d) { return d; }
template <typename T> T GetUintProperty(const std::string &, T d, T = T(~0)) { return d; }
template <typename T> T GetIntProperty(const std::string &, T d, T = T(), T = T()) { return d; }
inline bool GetBoolProperty(const std::string &, bool d) { return d; }
inline bool SetProperty(const std::string &, const std::string &) { return true; }
}  // namespace android::base
