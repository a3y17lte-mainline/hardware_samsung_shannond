// SPDX-License-Identifier: GPL-2.0-or-later
//
// Host stand-ins for the NDK binder classes the radio HAL uses: enough to
// compile against, nothing that runs.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

typedef int32_t binder_status_t;
enum { STATUS_OK = 0 };
struct AIBinder;

namespace ndk {

class ScopedAStatus {
  public:
    static ScopedAStatus ok() { return {}; }
    static ScopedAStatus fromServiceSpecificError(int32_t) { return {}; }
    static ScopedAStatus fromExceptionCode(int32_t) { return {}; }
    bool isOk() const { return true; }
};

struct ScopedFileDescriptor {
    int fd = -1;
    bool operator==(const ScopedFileDescriptor &) const = default;
};

class SpAIBinder {
  public:
    AIBinder *get() const { return nullptr; }
};

class SharedRefBase : public std::enable_shared_from_this<SharedRefBase> {
  public:
    virtual ~SharedRefBase() = default;
    template <typename T, typename... Args> static std::shared_ptr<T> make(Args &&...args)
    {
        return std::make_shared<T>(std::forward<Args>(args)...);
    }
    template <typename T> std::shared_ptr<T> ref()
    {
        return std::static_pointer_cast<T>(shared_from_this());
    }
};

class ICInterface : public SharedRefBase {
  public:
    virtual SpAIBinder asBinder() { return {}; }
    virtual bool isRemote() { return false; }
};

template <typename I> class BnCInterface : public I {};

}  // namespace ndk
