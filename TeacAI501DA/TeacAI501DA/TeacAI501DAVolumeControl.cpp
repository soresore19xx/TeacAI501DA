//
//  TeacAI501DAVolumeControl.cpp
//  TeacAI501DA
//
// === Claude origin ===
// Created/placed by Anthropic Claude Code at: 2026-07-07-221500
// v31: software volume plumbing. The framework's default handlers commit the
// new value (and notify the host); afterwards the current dB value is
// forwarded to the driver, which folds it into one atomic Q16 gain read by
// the isoch copy path. GetDecibelValue() is read *after* the super call so
// scalar-axis changes report the framework's own dB mapping.
// ====================
//

#include <os/log.h>
#include <DriverKit/IOLib.h>

#include "TeacAI501DAVolumeControl.h"
#include "TeacAI501DA.h"

struct TeacAI501DAVolumeControl_IVars
{
    TeacAI501DA *fDriver;
};

bool
TeacAI501DAVolumeControl::init(IOUserAudioDriver* in_driver,
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
    ivars = IONewZero(TeacAI501DAVolumeControl_IVars, 1);
    return ivars != nullptr;
}

void
TeacAI501DAVolumeControl::free()
{
    IOSafeDeleteNULL(ivars, TeacAI501DAVolumeControl_IVars, 1);
    IOUserAudioLevelControl::free();
}

void
TeacAI501DAVolumeControl::SetDriverRef(TeacAI501DA* in_driver)
{
    ivars->fDriver = in_driver;
}

kern_return_t
TeacAI501DAVolumeControl::HandleChangeScalarValue(float in_scalar_value)
{
    kern_return_t ret = IOUserAudioLevelControl::HandleChangeScalarValue(in_scalar_value);
    if (ret == kIOReturnSuccess && ivars && ivars->fDriver) {
        ivars->fDriver->UpdateOutputVolume(GetDecibelValue());
    }
    return ret;
}

kern_return_t
TeacAI501DAVolumeControl::HandleChangeDecibelValue(float in_decibel_value)
{
    kern_return_t ret = IOUserAudioLevelControl::HandleChangeDecibelValue(in_decibel_value);
    if (ret == kIOReturnSuccess && ivars && ivars->fDriver) {
        ivars->fDriver->UpdateOutputVolume(GetDecibelValue());
    }
    return ret;
}
