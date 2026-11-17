#pragma once
#include <cstdint>
#include <ctime>
struct prop_info;
inline const prop_info *__system_property_find(const char *) { return nullptr; }
inline bool __system_property_wait(const prop_info *, uint32_t, uint32_t *, const struct timespec *) { return true; }
