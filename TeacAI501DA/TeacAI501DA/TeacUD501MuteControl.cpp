//
//  TeacUD501MuteControl.cpp
//  TeacAI501DA
//
// === Claude origin ===
// Created/placed by Anthropic Claude Code at: 2026-11-22-133000
// UD-501V2 software mute plumbing: forwards the committed state to TeacUD501.
// ====================
//

#include <os/log.h>
#include <string.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSData.h>

#include "TeacUD501MuteControl.h"
#include "TeacUD501.h"

struct TeacUD501MuteControl_IVars
{
    TeacUD501 *fDriver;
    bool       fCleanLogged;
};

bool
TeacUD501MuteControl::init(IOUserAudioDriver* in_driver,
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
    ivars = IONewZero(TeacUD501MuteControl_IVars, 1);
    return ivars != nullptr;
}

void
TeacUD501MuteControl::free()
{
    IOSafeDeleteNULL(ivars, TeacUD501MuteControl_IVars, 1);
    IOUserAudioBooleanControl::free();
}

void
TeacUD501MuteControl::SetDriverRef(TeacUD501* in_driver)
{
    ivars->fDriver = in_driver;
}

kern_return_t
TeacUD501MuteControl::HandleChangeControlValue(bool in_control_value)
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
TeacUD501MuteControl::_GetPropertyData(const IOUserAudioObjectPropertyAddress* in_prop_addr,
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
            os_log(OS_LOG_DEFAULT, "TeacUD501: mute value read raw=0x%08x -> %u (upper bytes cleared; logged once)", raw, clean);
        }
    }
    return ret;
}
