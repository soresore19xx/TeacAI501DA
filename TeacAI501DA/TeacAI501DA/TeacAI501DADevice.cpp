//
//  TeacAI501DADevice.cpp
//  TeacAI501DA
//
// === Claude origin ===
// Created/placed by Anthropic Claude Code at: 2026-07-03-201500
// v17: sample-rate change plumbing. HandleChangeSampleRate stores the pending
// rate and requests a configuration change; PerformDeviceConfigurationChange
// (called by the host with IO stopped) reconfigures the USB side via the
// driver, then commits the rate to AudioDriverKit.
// ====================
//

#include <os/log.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSString.h>

#include "TeacAI501DADevice.h"
#include "TeacAI501DA.h"

#define LOG OS_LOG_DEFAULT
#define LOG_PREFIX "TeacAI501DA: "

struct TeacAI501DADevice_IVars
{
    TeacAI501DA *fDriver;
};

bool
TeacAI501DADevice::init(IOUserAudioDriver* in_driver,
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
    ivars = IONewZero(TeacAI501DADevice_IVars, 1);
    return ivars != nullptr;
}

void
TeacAI501DADevice::free()
{
    IOSafeDeleteNULL(ivars, TeacAI501DADevice_IVars, 1);
    IOUserAudioDevice::free();
}

void
TeacAI501DADevice::SetDriverRef(TeacAI501DA* in_driver)
{
    ivars->fDriver = in_driver;
}

kern_return_t
TeacAI501DADevice::PerformDeviceConfigurationChange(uint64_t in_change_action,
                                                    OSObject* in_change_info)
{
    kern_return_t ret = IOUserAudioDevice::PerformDeviceConfigurationChange(in_change_action,
                                                                            in_change_info);
    // v18: after the framework applied the change (host IO is stopped here),
    // bring the USB side in line with the current nominal rate. No-op when
    // the rate is unchanged.
    if (ret == kIOReturnSuccess && ivars->fDriver) {
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
