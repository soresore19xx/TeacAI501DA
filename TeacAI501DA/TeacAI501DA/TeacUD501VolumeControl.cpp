//
//  TeacUD501VolumeControl.cpp
//  TeacAI501DA
//
// === Claude origin ===
// Created/placed by Anthropic Claude Code at: 2026-11-22-133000
// UD-501V2 software volume plumbing: the framework's default handlers commit
// the value, then the current dB value is forwarded to TeacUD501.
// ====================
//

#include <os/log.h>
#include <DriverKit/IOLib.h>

#include "TeacUD501VolumeControl.h"
#include "TeacUD501.h"

struct TeacUD501VolumeControl_IVars
{
    TeacUD501 *fDriver;
};

bool
TeacUD501VolumeControl::init(IOUserAudioDriver* in_driver,
                             bool in_is_settable,
                             float in_decibel_value,
                             IOUserAudioLevelControlRange in_decibel_range,
                             IOUserAudioObjectPropertyElement in_control_element,
                             IOUserAudioObjectPropertyScope in_control_scope,
                             IOUserAudioClassID in_control_class_id)
{
    if (!IOUserAudioLevelControl::init(in_driver, in_is_settable, in_decibel_value,
                                       in_decibel_range, in_control_element,
                                       in_control_scope, in_control_class_id)) {
        return false;
    }
    ivars = IONewZero(TeacUD501VolumeControl_IVars, 1);
    return ivars != nullptr;
}

void
TeacUD501VolumeControl::free()
{
    IOSafeDeleteNULL(ivars, TeacUD501VolumeControl_IVars, 1);
    IOUserAudioLevelControl::free();
}

void
TeacUD501VolumeControl::SetDriverRef(TeacUD501* in_driver)
{
    ivars->fDriver = in_driver;
}

kern_return_t
TeacUD501VolumeControl::HandleChangeScalarValue(float in_scalar_value)
{
    kern_return_t ret = IOUserAudioLevelControl::HandleChangeScalarValue(in_scalar_value);
    if (ret == kIOReturnSuccess && ivars && ivars->fDriver) {
        ivars->fDriver->UpdateOutputVolume(GetDecibelValue());
    }
    return ret;
}

kern_return_t
TeacUD501VolumeControl::HandleChangeDecibelValue(float in_decibel_value)
{
    kern_return_t ret = IOUserAudioLevelControl::HandleChangeDecibelValue(in_decibel_value);
    if (ret == kIOReturnSuccess && ivars && ivars->fDriver) {
        ivars->fDriver->UpdateOutputVolume(GetDecibelValue());
    }
    return ret;
}
