//
//  TeacAI501DAMuteControl.cpp
//  TeacAI501DA
//
// === Claude origin ===
// Created/placed by Anthropic Claude Code at: 2026-07-07-221500
// v31: software mute plumbing. The framework's default handler commits the
// new value; afterwards the current state is forwarded to the driver, which
// folds it into the atomic Q16 gain read by the isoch copy path.
// ====================
//

#include <os/log.h>
#include <DriverKit/IOLib.h>

#include "TeacAI501DAMuteControl.h"
#include "TeacAI501DA.h"

struct TeacAI501DAMuteControl_IVars
{
    TeacAI501DA *fDriver;
};

bool
TeacAI501DAMuteControl::init(IOUserAudioDriver* in_driver,
                             bool in_is_settable,
                             bool in_control_value,
                             IOUserAudioObjectPropertyElement in_control_element,
                             IOUserAudioObjectPropertyScope in_control_scope,
                             IOUserAudioClassID in_control_class_id)
{
    if (!IOUserAudioBooleanControl::init(in_driver, in_is_settable, in_control_value,
                                         in_control_element, in_control_scope,
                                         in_control_class_id)) {
        return false;
    }
    ivars = IONewZero(TeacAI501DAMuteControl_IVars, 1);
    return ivars != nullptr;
}

void
TeacAI501DAMuteControl::free()
{
    IOSafeDeleteNULL(ivars, TeacAI501DAMuteControl_IVars, 1);
    IOUserAudioBooleanControl::free();
}

void
TeacAI501DAMuteControl::SetDriverRef(TeacAI501DA* in_driver)
{
    ivars->fDriver = in_driver;
}

kern_return_t
TeacAI501DAMuteControl::HandleChangeControlValue(bool in_control_value)
{
    kern_return_t ret = IOUserAudioBooleanControl::HandleChangeControlValue(in_control_value);
    if (ret == kIOReturnSuccess && ivars && ivars->fDriver) {
        ivars->fDriver->UpdateOutputMute(GetControlValue());
    }
    return ret;
}
