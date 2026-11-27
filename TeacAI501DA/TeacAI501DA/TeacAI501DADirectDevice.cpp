//
//  TeacAI501DADirectDevice.cpp
//  TeacAI501DA
//
// === Claude origin ===
// Created/placed by Anthropic Claude Code at: 2026-11-24-102000
// "(Direct)" device: PerformDeviceConfigurationChange hands over to the driver,
// which copies the main device's rate onto this device.
// ====================
//

#include <os/log.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSString.h>

#include "TeacAI501DADirectDevice.h"
#include "TeacAI501DA.h"

struct TeacAI501DADirectDevice_IVars
{
    TeacAI501DA *fDriver;
};

bool
TeacAI501DADirectDevice::init(IOUserAudioDriver* in_driver,
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
    ivars = IONewZero(TeacAI501DADirectDevice_IVars, 1);
    return ivars != nullptr;
}

void
TeacAI501DADirectDevice::free()
{
    IOSafeDeleteNULL(ivars, TeacAI501DADirectDevice_IVars, 1);
    IOUserAudioDevice::free();
}

void
TeacAI501DADirectDevice::SetDriverRef(TeacAI501DA* in_driver)
{
    ivars->fDriver = in_driver;
}

kern_return_t
TeacAI501DADirectDevice::PerformDeviceConfigurationChange(uint64_t in_change_action,
                                                          OSObject* in_change_info)
{
    kern_return_t ret = IOUserAudioDevice::PerformDeviceConfigurationChange(in_change_action,
                                                                            in_change_info);
    if (ivars && ivars->fDriver) {
        ivars->fDriver->DirectFollowRate(in_change_action == TEAC_DIRECT_FOLLOW_ACTION);
    }
    return ret;
}
