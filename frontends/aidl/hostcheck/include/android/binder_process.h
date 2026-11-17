#pragma once
#include <cstdint>
inline bool ABinderProcess_setThreadPoolMaxThreadCount(uint32_t) { return true; }
inline void ABinderProcess_startThreadPool() {}
inline void ABinderProcess_joinThreadPool() {}
