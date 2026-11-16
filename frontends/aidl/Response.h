// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <aidl/android/hardware/radio/RadioError.h>
#include <aidl/android/hardware/radio/RadioIndicationType.h>
#include <aidl/android/hardware/radio/RadioResponseInfo.h>
#include <aidl/android/hardware/radio/RadioResponseType.h>

namespace shannond::hal {

using ::aidl::android::hardware::radio::RadioError;
using ::aidl::android::hardware::radio::RadioIndicationType;
using ::aidl::android::hardware::radio::RadioResponseInfo;
using ::aidl::android::hardware::radio::RadioResponseType;

constexpr RadioIndicationType kUnsolicited = RadioIndicationType::UNSOLICITED;

inline RadioResponseInfo responseInfo(int32_t serial, RadioError error)
{
    RadioResponseInfo info;
    info.type = RadioResponseType::SOLICITED;
    info.serial = serial;
    info.error = error;
    return info;
}

inline RadioResponseInfo ok(int32_t serial) { return responseInfo(serial, RadioError::NONE); }
inline RadioResponseInfo notSupported(int32_t serial)
{
    return responseInfo(serial, RadioError::REQUEST_NOT_SUPPORTED);
}
inline RadioResponseInfo notAvailable(int32_t serial)
{
    return responseInfo(serial, RadioError::RADIO_NOT_AVAILABLE);
}
inline RadioResponseInfo failed(int32_t serial) { return responseInfo(serial, RadioError::GENERIC_FAILURE); }

}  // namespace shannond::hal
