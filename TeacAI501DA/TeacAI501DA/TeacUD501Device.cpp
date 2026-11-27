//
//  TeacUD501Device.cpp
//  TeacAI501DA
//
// === Claude origin ===
// Created/placed by Anthropic Claude Code at: 2026-11-22-133000
// UD-501V2 sample-rate change plumbing: after the framework applied a change
// (host IO stopped), bring the USB side in line with the current nominal rate.
// ====================
//

#include <os/log.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSString.h>

#include "TeacUD501Device.h"
#include "TeacUD501.h"

#define LOG OS_LOG_DEFAULT
#define LOG_PREFIX "TeacUD501: "

struct TeacUD501Device_IVars
{
    TeacUD501 *fDriver;
};

bool
TeacUD501Device::init(IOUserAudioDriver* in_driver,
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
    ivars = IONewZero(TeacUD501Device_IVars, 1);
    return ivars != nullptr;
}

void
TeacUD501Device::free()
{
    IOSafeDeleteNULL(ivars, TeacUD501Device_IVars, 1);
    IOUserAudioDevice::free();
}

void
TeacUD501Device::SetDriverRef(TeacUD501* in_driver)
{
    ivars->fDriver = in_driver;
}

kern_return_t
TeacUD501Device::PerformDeviceConfigurationChange(uint64_t in_change_action,
                                                  OSObject* in_change_info)
{
    kern_return_t ret = IOUserAudioDevice::PerformDeviceConfigurationChange(in_change_action,
                                                                            in_change_info);
    if (ret == kIOReturnSuccess && ivars->fDriver) {
        // "(Direct)" was set to another rate by a client: take that rate here, then
        // the USB side below switches to it
        if (in_change_action == UD501_MAIN_FOLLOW_ACTION) {
            uint32_t pending = ivars->fDriver->PendingMainRate();
            if (pending) {
                SetSampleRate((double)pending);
            }
        }
        uint32_t hz = (uint32_t)(GetSampleRate() + 0.5);
        os_log(LOG, LOG_PREFIX "PerformDeviceConfigurationChange action=%llu rate=%u",
               in_change_action, hz);
        kern_return_t usbRet = ivars->fDriver->ReconfigureForRate(hz);
        if (usbRet != kIOReturnSuccess) {
            os_log(LOG, LOG_PREFIX "ReconfigureForRate(%u) failed (0x%x)", hz, usbRet);
        }
    }
    return ret;
}
