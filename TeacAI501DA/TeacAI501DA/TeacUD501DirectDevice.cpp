//
//  TeacUD501DirectDevice.cpp
//  TeacAI501DA
//
// === Claude origin ===
// Created/placed by Anthropic Claude Code at: 2026-11-24-140500
// UD-501V2 "(Direct)" device: PerformDeviceConfigurationChange hands over to the
// TeacUD501 driver, which keeps this device and the main device on one rate.
// ====================
//

#include <os/log.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSString.h>

#include "TeacUD501DirectDevice.h"
#include "TeacUD501.h"

struct TeacUD501DirectDevice_IVars
{
    TeacUD501 *fDriver;
};

bool
TeacUD501DirectDevice::init(IOUserAudioDriver* in_driver,
                            bool in_supports_prewarming,
                            OSString* in_device_uid,
                            OSString* in_model_uid,
                            OSString* in_manufacturer_uid,
                            uint32_t in_zero_timestamp_period)
{
    if (!IOUserAudioDevice::init(in_driver, in_supports_prewarming, in_device_uid,
                                 in_model_uid, in_manufacturer_uid,
                                 in_zero_timestamp_period)) {
        return false;
    }
    ivars = IONewZero(TeacUD501DirectDevice_IVars, 1);
    return ivars != nullptr;
}

void
TeacUD501DirectDevice::free()
{
    IOSafeDeleteNULL(ivars, TeacUD501DirectDevice_IVars, 1);
    IOUserAudioDevice::free();
}

void
TeacUD501DirectDevice::SetDriverRef(TeacUD501* in_driver)
{
    ivars->fDriver = in_driver;
}

kern_return_t
TeacUD501DirectDevice::PerformDeviceConfigurationChange(uint64_t in_change_action,
                                                        OSObject* in_change_info)
{
    kern_return_t ret = IOUserAudioDevice::PerformDeviceConfigurationChange(in_change_action,
                                                                            in_change_info);
    if (ivars && ivars->fDriver) {
        ivars->fDriver->DirectFollowRate(in_change_action == UD501_DIRECT_FOLLOW_ACTION);
    }
    return ret;
}
