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
#include <string.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSData.h>

#include "TeacAI501DAMuteControl.h"
#include "TeacAI501DA.h"

struct TeacAI501DAMuteControl_IVars
{
    TeacAI501DA *fDriver;
    bool         fCleanLogged;
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

// kAudioBooleanControlPropertyValue ('bcvl').
static constexpr IOUserAudioObjectPropertySelector kSelBooleanControlValue = 0x6263766C;

// The host reads the mute state as a UInt32. The base class fills only the
// low byte and leaves the other three as leftover memory (seen on Intel with
// macOS 12.7 / 15.8 / 26.7 and on Apple silicon with 27.2), so the value
// is rarely 0 or 1 exactly and coreaudiod re-applies a wrong mute state after a
// rate change or reconnect. The low byte is the real value; send it as 0/1.
kern_return_t
TeacAI501DAMuteControl::_GetPropertyData(const IOUserAudioObjectPropertyAddress* in_prop_addr,
                                         OSData* in_qualifier_data,
                                         OSData** out_data)
{
    kern_return_t ret = IOUserAudioBooleanControl::_GetPropertyData(in_prop_addr, in_qualifier_data, out_data);
    if (ret != kIOReturnSuccess || in_prop_addr == nullptr || out_data == nullptr || *out_data == nullptr) {
        return ret;
    }
    if (in_prop_addr->mSelector != kSelBooleanControlValue || (*out_data)->getLength() != sizeof(uint32_t)) {
        return ret;
    }
    uint32_t raw = 0;
    memcpy(&raw, (*out_data)->getBytesNoCopy(), sizeof(raw));
    uint32_t clean = (raw & 0xFFu) ? 1u : 0u;
    if (raw != clean) {
        OSData *fixed = OSData::withBytes(&clean, sizeof(clean));
        if (fixed != nullptr) {
            (*out_data)->release();
            *out_data = fixed;
        }
        if (ivars && !ivars->fCleanLogged) {
            ivars->fCleanLogged = true;
            os_log(OS_LOG_DEFAULT, "TeacAI501DA: mute value read raw=0x%08x -> %u (upper bytes cleared; logged once)", raw, clean);
        }
    }
    return ret;
}
