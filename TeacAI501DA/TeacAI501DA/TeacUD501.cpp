//
//  TeacUD501.cpp
//  TeacAI501DA
//
// === Claude origin ===
// Created/placed by Anthropic Claude Code at: 2026-11-22-133000
// TEAC UD-501V2 (0x0644:0x8043) USB audio bridge with software volume/mute.
// Structure follows the AI-501DA class (device-level ownership, pipelined
// isoch OUT paced by explicit feedback, ZTS from completed samples, silence
// fill on supply stall, quiesced Stop), re-done for the UD-501V2 topology.
// All helpers live in an anonymous namespace so nothing here can be coalesced
// with same-named symbols of the AI-501DA translation unit at link time.
// ====================
//
//  UD-501V2 topology (lsusb, 2026-11-22):
//    IF#0 AudioControl (UAC2): ClockSource 41 (internal, freq r/w) -> ClockSelector 40
//         -> InputTerminal 2 -> FeatureUnit 10 (mute + per-channel volume, inaudible)
//         -> OutputTerminal 20 (Digital Audio Interface)
//    IF#1 AudioStreaming: alt1 16bit/2B, alt2 24bit/4B, alt3 32bit/4B (Linux uses
//         alt3 as native DSD after a vendor mode switch). OUT EP 0x01 iso async
//         1024B bInterval 1; feedback EP 0x81 4B bInterval 4 (1 ms).
//    IF#2 HID (EP 0x84 interrupt IN).
//  The USB side is a TI C67x DSP running TEAC firmware — not the Tenor TE8802L of
//  the AI-501DA, so none of the AI-501DA feedback workarounds are assumed.
//

#include <os/log.h>
#include <string.h>

#include <DriverKit/IOUserServer.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSString.h>
#include <USBDriverKit/IOUSBHostDevice.h>
#include <USBDriverKit/IOUSBHostInterface.h>
#include <USBDriverKit/IOUSBHostPipe.h>
#include <USBDriverKit/USBDriverKitDefs.h>
#include <AudioDriverKit/IOUserAudioDevice.h>
#include <AudioDriverKit/IOUserAudioStream.h>
#include <AudioDriverKit/IOUserAudioClockDevice.h>
#include <AudioDriverKit/AudioDriverKitTypes.h>

#include "TeacUD501.h"
#include "TeacUD501Device.h"
#include "TeacUD501VolumeControl.h"
#include "TeacUD501MuteControl.h"

#define LOG OS_LOG_DEFAULT
#define LOG_PREFIX "TeacUD501: "

// Same build-variant switch as the AI-501DA class (one flag for the whole dext).
#ifndef TEAC_FIXED_VOLUME
#define TEAC_FIXED_VOLUME             0
#endif

namespace {

// ---- topology -------------------------------------------------------------
constexpr uint8_t  kConfigValue        = 1;
constexpr uint8_t  kControlIfNum       = 0;     // UAC2 AudioControl
constexpr uint8_t  kStreamingIfNum     = 1;     // UAC2 AudioStreaming
constexpr uint8_t  kClockSourceID      = 41;
constexpr uint8_t  kStreamingAlt       = 2;     // 24-bit samples in 4-byte subslots
constexpr uint8_t  kOutEP              = 0x01;
constexpr uint8_t  kFbEP               = 0x81;
constexpr uint8_t  kUac2Cur            = 0x01;
constexpr uint8_t  kUac2Range          = 0x02;
constexpr uint8_t  kUac2SamFreqControl = 0x01;

// Linux (sound/usb quirks for 0644:8043) waits 20 ms after control messages and
// 50 ms after interface changes; the TEAC DSP firmware needs the settle time.
constexpr uint32_t kCtlMsgDelayMs      = 20;
constexpr uint32_t kIfaceDelayMs       = 50;

// Linux issues this vendor request (bmRequestType 0x41, bRequest 0, wIndex 1)
// before every stream start: wValue 0 = PCM/DoP mode, 1 = native DSD mode.
// Sending PCM mode guards against a unit left in DSD mode by another host.
constexpr bool     kSendPcmModeCommand = true;

// ---- stream geometry --------------------------------------------------------
constexpr uint32_t kNumOutFrames       = 8;     // µframes per transfer (= 1 ms)
constexpr uint32_t kOutBytesPerFrame   = 1024;  // wMaxPacketSize; real max is ~432 @384k
constexpr uint32_t kOutBufSize         = kNumOutFrames * kOutBytesPerFrame;
constexpr uint32_t kOutXfersInFlight   = 4;
constexpr uint32_t kWireBytesPerFrame  = 8;     // 2ch × 4-byte subslot
constexpr uint32_t kRingBytesPerFrame  = 6;     // CoreAudio side: packed 24-bit × 2ch
constexpr uint32_t kBaselineRate       = 48000;
constexpr uint32_t kMicroframesPerSec  = 8000;

// Candidate nominal rates; the published list is filtered by the clock's RANGE.
constexpr uint32_t kCandidateRates[]   = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
constexpr uint32_t kNumCandidateRates  = sizeof(kCandidateRates) / sizeof(kCandidateRates[0]);

// ZTS period == ring frames (maudio rule). 98304 frames = 256 ms @384k, the same
// head room the AI-501DA has at its 192k maximum.
constexpr uint32_t kZtsPeriodFrames    = 98304;

inline uint32_t
SafetyOffsetForRate(uint32_t rate_hz)
{
    uint32_t off = (uint32_t)(((uint64_t)rate_hz * 40) / 1000);  // 40 ms
    return off < 1024 ? 1024 : off;
}

inline uint32_t
NominalFbQ16(uint32_t rate_hz)
{
    return (uint32_t)(((uint64_t)rate_hz << 16) / kMicroframesPerSec);
}

// ---- software volume ---------------------------------------------------------
constexpr float    kVolumeMinDb        = -60.0f;
constexpr float    kVolumeMaxDb        = 0.0f;
constexpr uint32_t kGainQ16Unity       = 65536u;

// 10^(dB/20) in Q16, -60 dB .. 0 dB in 0.5 dB steps (DriverKit has no libm).
constexpr uint32_t kGainTableQ16[121] = {
    66u, 69u, 74u, 78u, 83u, 87u, 93u, 98u,
    104u, 110u, 117u, 123u, 131u, 139u, 147u, 155u,
    165u, 174u, 185u, 196u, 207u, 220u, 233u, 246u,
    261u, 276u, 293u, 310u, 328u, 348u, 369u, 390u,
    414u, 438u, 464u, 491u, 521u, 551u, 584u, 619u,
    655u, 694u, 735u, 779u, 825u, 874u, 926u, 981u,
    1039u, 1100u, 1165u, 1234u, 1308u, 1385u, 1467u, 1554u,
    1646u, 1744u, 1847u, 1957u, 2072u, 2195u, 2325u, 2463u,
    2609u, 2764u, 2927u, 3101u, 3285u, 3479u, 3685u, 3904u,
    4135u, 4380u, 4640u, 4915u, 5206u, 5514u, 5841u, 6187u,
    6554u, 6942u, 7353u, 7789u, 8250u, 8739u, 9257u, 9806u,
    10387u, 11002u, 11654u, 12345u, 13076u, 13851u, 14672u, 15541u,
    16462u, 17437u, 18471u, 19565u, 20724u, 21952u, 23253u, 24631u,
    26090u, 27636u, 29274u, 31008u, 32846u, 34792u, 36854u, 39037u,
    41350u, 43801u, 46396u, 49145u, 52057u, 55142u, 58409u, 61870u,
    65536u,
};

inline uint32_t
GainQ16FromDecibel(float db)
{
    if (db <= kVolumeMinDb) return 0;
    if (db >= kVolumeMaxDb) return kGainQ16Unity;
    int idx = (int)((db - kVolumeMinDb) * 2.0f + 0.5f);
    if (idx < 0) idx = 0;
    if (idx > 120) idx = 120;
    return kGainTableQ16[idx];
}

// DoP: the top byte of each 24-bit sample is 0x05/0xFA, identical on L/R and
// alternating every frame. Gain would destroy the markers (DSD turns to noise),
// so chunks carrying DoP bypass the volume. A run of 16 consecutive marker
// frames is required — PCM practically never produces that. The decision is
// per 1 ms chunk with no hold-over, so PCM that follows DoP at the same rate is
// attenuated again from the very next chunk (no full-level burst).
constexpr uint32_t kDopMinRun          = 16;

bool
ChunkLooksLikeDoP(const uint8_t *out, uint32_t frames)
{
    uint32_t run = 0;
    uint8_t prev = 0;
    for (uint32_t f = 0; f < frames; f++) {
        uint8_t l = out[f * kWireBytesPerFrame + 3];
        uint8_t r = out[f * kWireBytesPerFrame + 7];
        bool marker = (l == r) && (l == 0x05 || l == 0xFA);
        if (marker && (run == 0 || l != prev)) {
            run++;
            if (run >= kDopMinRun) return true;
        } else {
            run = marker ? 1 : 0;
        }
        prev = l;
    }
    return false;
}

inline uint32_t
ReadLE32(const volatile void *p)
{
    const volatile uint8_t *b = (const volatile uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

inline IOUserAudioStreamBasicDescription
MakeFormat24(double rate)
{
    IOUserAudioStreamBasicDescription fmt = {
        .mSampleRate       = rate,
        .mFormatID         = IOUserAudioFormatID::LinearPCM,
        .mFormatFlags      = (IOUserAudioFormatFlags)
            (IOUserAudioFormatFlags::FormatFlagIsSignedInteger
           | IOUserAudioFormatFlags::FormatFlagIsPacked),
        .mBytesPerPacket   = kRingBytesPerFrame,
        .mFramesPerPacket  = 1,
        .mBytesPerFrame    = kRingBytesPerFrame,
        .mChannelsPerFrame = 2,
        .mBitsPerChannel   = 24,
        .mReserved         = 0,
    };
    return fmt;
}

} // namespace

struct TeacUD501_IVars
{
    // USB resources
    IOUSBHostDevice          *fDevice;
    IOUSBHostInterface       *fInterface;
    IOUSBHostPipe            *fFbPipe;
    IOUSBHostPipe            *fOutPipe;
    IOBufferMemoryDescriptor *fFbDataBuf;
    IOBufferMemoryDescriptor *fFbFrameList;
    OSAction                 *fFbAction;
    IOBufferMemoryDescriptor *fOutDataBufs[kOutXfersInFlight];
    IOBufferMemoryDescriptor *fOutFrameLists[kOutXfersInFlight];
    OSAction                 *fOutAction;
    uint64_t                  fOutFrameNumber;
    uint32_t                  fOutSlot;

    // rates published to CoreAudio (filtered by the clock's RANGE)
    uint32_t                  fRates[kNumCandidateRates];
    uint32_t                  fNumRates;

    // pacing (Q16.16 samples/µframe from explicit feedback)
    uint32_t                  fRateHz;
    bool                      fReconfiguring;
    uint32_t                  fFbCurrentQ16;
    uint32_t                  fFbAccumQ16;
    uint32_t                  fSamplesPerUF;
    uint32_t                  fFbMode;          // 0 = not yet seen, 1 = raw Q16.16, 2 = raw + 1.0
    uint64_t                  fFbRejected;

    // CoreAudio
    IOUserAudioDevice        *fAudioDevice;
    IOUserAudioStream        *fOutputStream;
    TeacUD501VolumeControl   *fVolumeControl;
    TeacUD501MuteControl     *fMuteControl;
    volatile uint32_t         fGainQ16;
    volatile uint32_t         fVolumeGainQ16;
    volatile bool             fOutputMuted;
    uint64_t                  fDopChunks;       // chunks that bypassed the gain because they carried DoP
    IOBufferMemoryDescriptor *fCoreAudioBuf;
    void                     *fCoreAudioBufPtr;
    size_t                    fCoreAudioBufLen;

    // USB <-> CoreAudio bridging
    uint64_t                  fUsbCumulativeSamples;
    uint64_t                  fUsbCompletedSamples;
    uint64_t                  fCoreAudioWriteEndSample;
    uint64_t                  fNextZtsSample;
    uint64_t                  fPrevWriteEnd;

    // diagnostics (same meaning as the AI-501DA counters)
    uint64_t                  fFbCount;
    uint64_t                  fOutCount;
    uint64_t                  fOutBadFrames;
    uint64_t                  fOutShortFrames;
    uint32_t                  fPeakLevel;
    uint32_t                  fLastBadStatus;
    bool                      fFirstBadLogged;
    uint64_t                  fShortDeficitSamples;
    uint64_t                  fMaxDivergence;
    uint64_t                  fRingOverrunEvents;
    uint64_t                  fResyncCount;
    uint64_t                  fRingGuardReanchors;
    uint64_t                  fSilenceFilledSamples;
    bool                      fUnderrunActive;

    // teardown / reconfigure quiesce (see the AI-501DA v28/v29 notes)
    volatile bool             fStopping;
    volatile uint32_t         fIoInFlight;
    volatile uint32_t         fInReconfigure;
    volatile bool             fRearmAfterDrain;
};

namespace {

inline void
IoInc(TeacUD501_IVars *iv)
{
    __atomic_add_fetch(&iv->fIoInFlight, 1, __ATOMIC_ACQ_REL);
}

inline void
IoDec(TeacUD501_IVars *iv)
{
    __atomic_sub_fetch(&iv->fIoInFlight, 1, __ATOMIC_ACQ_REL);
}

kern_return_t Ud501ArmUsbStreaming(TeacUD501_IVars *iv);

struct Ud501IoToken {
    TeacUD501_IVars *iv;
    explicit Ud501IoToken(TeacUD501_IVars *in_iv) : iv(in_iv) {}
    ~Ud501IoToken() {
        if (!iv) {
            return;
        }
        if (__atomic_sub_fetch(&iv->fIoInFlight, 1, __ATOMIC_ACQ_REL) == 0 &&
            iv->fRearmAfterDrain && !iv->fStopping) {
            iv->fRearmAfterDrain = false;
            iv->fReconfiguring = false;
            kern_return_t r = Ud501ArmUsbStreaming(iv);
            os_log(LOG, LOG_PREFIX "drain complete — deferred re-arm (ret=0x%x)", r);
        }
    }
};

struct Ud501ReconfToken {
    volatile uint32_t *flag;
    explicit Ud501ReconfToken(volatile uint32_t *in_flag) : flag(in_flag) {
        __atomic_store_n(flag, 1, __ATOMIC_SEQ_CST);
    }
    ~Ud501ReconfToken() { __atomic_store_n(flag, 0, __ATOMIC_SEQ_CST); }
};

kern_return_t
Ud501ScheduleRearmAfterDrain(TeacUD501_IVars *iv)
{
    if (__atomic_load_n(&iv->fIoInFlight, __ATOMIC_ACQUIRE) == 0) {
        iv->fReconfiguring = false;
        return Ud501ArmUsbStreaming(iv);
    }
    iv->fRearmAfterDrain = true;
    return kIOReturnSuccess;
}

inline uint32_t
NextOutSampleCount(TeacUD501_IVars *iv)
{
    iv->fFbAccumQ16 += iv->fFbCurrentQ16;
    uint32_t samples = iv->fFbAccumQ16 >> 16;
    iv->fFbAccumQ16 &= 0xFFFF;
    return samples;
}

void
FillOutFrameList(IOUSBIsochronousFrame *frames, uint32_t numFrames, TeacUD501_IVars *iv)
{
    for (uint32_t i = 0; i < numFrames; i++) {
        uint32_t samples = NextOutSampleCount(iv);
        iv->fSamplesPerUF = samples;
        frames[i].status        = kIOReturnSuccess;
        frames[i].requestCount  = samples * kWireBytesPerFrame;
        frames[i].completeCount = 0;
        frames[i].timeStamp     = 0;
    }
}

uint32_t
GetClockSampleRate(IOService *client, IOUSBHostDevice *device)
{
    IOBufferMemoryDescriptor *buf = NULL;
    if (IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn, 4, 4, &buf) != kIOReturnSuccess || !buf) {
        return 0;
    }
    uint16_t xfer = 0;
    uint32_t hz = 0;
    kern_return_t ret = device->DeviceRequest(client,
        0xA1,                                   // D2H | Class | Interface
        kUac2Cur,
        (uint16_t)(kUac2SamFreqControl << 8),
        (uint16_t)((kClockSourceID << 8) | kControlIfNum),
        4, buf, &xfer, 5000);
    IOSleep(kCtlMsgDelayMs);
    if (ret == kIOReturnSuccess && xfer == 4) {
        IOAddressSegment seg;
        if (buf->GetAddressRange(&seg) == kIOReturnSuccess) {
            hz = ReadLE32((const volatile void *)seg.address);
        }
    }
    OSSafeReleaseNULL(buf);
    return hz;
}

// UAC2 RANGE on the clock: log the subranges and publish the candidate rates
// they cover. Falls back to 44.1k..192k when the request fails.
void
BuildRateList(IOService *client, IOUSBHostDevice *device, TeacUD501_IVars *iv)
{
    iv->fNumRates = 0;
    IOBufferMemoryDescriptor *buf = NULL;
    if (IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn, 256, 4, &buf) == kIOReturnSuccess && buf) {
        uint16_t xfer = 0;
        kern_return_t ret = device->DeviceRequest(client,
            0xA1, kUac2Range,
            (uint16_t)(kUac2SamFreqControl << 8),
            (uint16_t)((kClockSourceID << 8) | kControlIfNum),
            256, buf, &xfer, 5000);
        IOSleep(kCtlMsgDelayMs);
        IOAddressSegment seg;
        if (ret == kIOReturnSuccess && xfer >= 2 && buf->GetAddressRange(&seg) == kIOReturnSuccess) {
            const uint8_t *p = (const uint8_t *)seg.address;
            uint16_t n = (uint16_t)(p[0] | (p[1] << 8));
            os_log(LOG, LOG_PREFIX "clock %u RANGE: %u subranges (xfer=%u)", kClockSourceID, n, xfer);
            for (uint32_t c = 0; c < kNumCandidateRates; c++) {
                uint32_t hz = kCandidateRates[c];
                for (uint16_t i = 0; i < n && (2u + 12u * i + 12u) <= xfer; i++) {
                    const uint8_t *r = p + 2 + 12 * i;
                    uint32_t mn = (uint32_t)(r[0] | (r[1] << 8) | (r[2] << 16) | ((uint32_t)r[3] << 24));
                    uint32_t mx = (uint32_t)(r[4] | (r[5] << 8) | (r[6] << 16) | ((uint32_t)r[7] << 24));
                    uint32_t rs = (uint32_t)(r[8] | (r[9] << 8) | (r[10] << 16) | ((uint32_t)r[11] << 24));
                    if (c == 0) {
                        os_log(LOG, LOG_PREFIX "clock range[%u]: min=%u max=%u res=%u", i, mn, mx, rs);
                    }
                    if (hz >= mn && hz <= mx && (rs == 0 || ((hz - mn) % rs) == 0)) {
                        iv->fRates[iv->fNumRates++] = hz;
                        break;
                    }
                }
            }
        } else {
            os_log(LOG, LOG_PREFIX "clock %u RANGE failed (0x%x xfer=%u)", kClockSourceID, ret, xfer);
        }
        OSSafeReleaseNULL(buf);
    }
    if (iv->fNumRates == 0) {
        for (uint32_t c = 0; c < kNumCandidateRates && kCandidateRates[c] <= 192000; c++) {
            iv->fRates[iv->fNumRates++] = kCandidateRates[c];
        }
        os_log(LOG, LOG_PREFIX "using fallback rate list (%u rates)", iv->fNumRates);
    }
    for (uint32_t i = 0; i < iv->fNumRates; i++) {
        os_log(LOG, LOG_PREFIX "published rate %u", iv->fRates[i]);
    }
}

kern_return_t
SendClockSampleRate(IOService *client, IOUSBHostDevice *device, uint32_t hz)
{
    IOBufferMemoryDescriptor *freqBuf = NULL;
    kern_return_t ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOut, 4, 4, &freqBuf);
    if (ret != kIOReturnSuccess || !freqBuf) return ret ? ret : kIOReturnNoMemory;
    IOAddressSegment seg;
    if (freqBuf->GetAddressRange(&seg) == kIOReturnSuccess) {
        uint8_t *p = (uint8_t *)seg.address;
        p[0] = (uint8_t)(hz & 0xFF);
        p[1] = (uint8_t)((hz >> 8) & 0xFF);
        p[2] = (uint8_t)((hz >> 16) & 0xFF);
        p[3] = (uint8_t)((hz >> 24) & 0xFF);
    }
    uint16_t xfer = 0;
    ret = device->DeviceRequest(client,
        0x21,                                   // H2D | Class | Interface
        kUac2Cur,
        (uint16_t)(kUac2SamFreqControl << 8),
        (uint16_t)((kClockSourceID << 8) | kControlIfNum),
        4, freqBuf, &xfer, 5000);
    IOSleep(kCtlMsgDelayMs);
    OSSafeReleaseNULL(freqBuf);
    uint32_t cur = GetClockSampleRate(client, device);
    os_log(LOG, LOG_PREFIX "SET_CUR sampleRate=%u ret=0x%x xfer=%u -> clk%u=%u",
           hz, ret, xfer, kClockSourceID, cur);
    if (ret == kIOReturnSuccess && cur != 0 && cur != hz) {
        os_log(LOG, LOG_PREFIX "clock snapped %u -> %u", hz, cur);
        ret = kIOReturnUnsupported;
    }
    return ret;
}

void
SendPcmModeCommand(IOService *client, IOUSBHostDevice *device)
{
    if (!kSendPcmModeCommand) {
        return;
    }
    uint16_t xfer = 0;
    kern_return_t ret = device->DeviceRequest(client,
        0x41,                                   // H2D | Vendor | Interface
        0,                                      // mode switch
        0,                                      // 0 = PCM/DoP
        kStreamingIfNum,
        0, NULL, &xfer, 5000);
    IOSleep(kCtlMsgDelayMs);
    os_log(LOG, LOG_PREFIX "vendor PCM-mode command ret=0x%x", ret);
}

kern_return_t
SelectAlt(IOUSBHostInterface *iface, uint8_t alt)
{
    kern_return_t ret = iface->SelectAlternateSetting(alt);
    IOSleep(kIfaceDelayMs);
    return ret;
}

kern_return_t
Ud501ArmUsbStreaming(TeacUD501_IVars *iv)
{
    if (__atomic_load_n(&iv->fStopping, __ATOMIC_SEQ_CST)) {
        return kIOReturnOffline;
    }
    IOAddressSegment flSeg;
    if (iv->fFbFrameList->GetAddressRange(&flSeg) == kIOReturnSuccess) {
        IOUSBIsochronousFrame *f = (IOUSBIsochronousFrame *)flSeg.address;
        memset(f, 0, sizeof(*f));
        f->requestCount = 4;
    }
    IoInc(iv);
    kern_return_t ret = iv->fFbPipe->IsochIO(iv->fFbDataBuf, iv->fFbFrameList, 0, iv->fFbAction);
    if (ret != kIOReturnSuccess) {
        IoDec(iv);
        os_log(LOG, LOG_PREFIX "fFbPipe->IsochIO failed (0x%x)", ret);
        return ret;
    }
    uint64_t frame = 0;
    ret = iv->fDevice->GetFrameNumber(&frame, NULL);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "GetFrameNumber failed (0x%x)", ret);
        return ret;
    }
    iv->fOutFrameNumber = frame + 4;
    iv->fOutSlot = 0;
    for (uint32_t s = 0; s < kOutXfersInFlight; s++) {
        IOAddressSegment ds;
        if (iv->fOutDataBufs[s]->GetAddressRange(&ds) == kIOReturnSuccess) {
            memset((void *)ds.address, 0, kOutBufSize);
        }
        IOAddressSegment fl;
        if (iv->fOutFrameLists[s]->GetAddressRange(&fl) == kIOReturnSuccess) {
            FillOutFrameList((IOUSBIsochronousFrame *)fl.address, kNumOutFrames, iv);
        }
        IoInc(iv);
        ret = iv->fOutPipe->IsochIO(iv->fOutDataBufs[s], iv->fOutFrameLists[s],
                                    iv->fOutFrameNumber, iv->fOutAction);
        if (ret != kIOReturnSuccess) {
            IoDec(iv);
            os_log(LOG, LOG_PREFIX "fOutPipe->IsochIO[%u]@%llu failed (0x%x)", s,
                   iv->fOutFrameNumber, ret);
            return ret;
        }
        iv->fOutFrameNumber++;
    }
    return kIOReturnSuccess;
}

bool
RateIsPublished(const TeacUD501_IVars *iv, uint32_t hz)
{
    for (uint32_t i = 0; i < iv->fNumRates; i++) {
        if (iv->fRates[i] == hz) return true;
    }
    return false;
}

} // namespace

void
TeacUD501::free()
{
    IOSafeDeleteNULL(ivars, TeacUD501_IVars, 1);
    super::free();
}

kern_return_t
IMPL(TeacUD501, Start)
{
    kern_return_t ret;

    os_log(LOG, LOG_PREFIX "Start() entered, provider=%p", provider);

    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "super Start failed (0x%x)", ret);
        return ret;
    }

    ivars = IONewZero(TeacUD501_IVars, 1);
    if (!ivars) {
        return kIOReturnNoMemory;
    }
    ivars->fNextZtsSample = kZtsPeriodFrames;
    ivars->fRateHz = kBaselineRate;
    ivars->fFbCurrentQ16 = NominalFbQ16(kBaselineRate);
    ivars->fSamplesPerUF = kBaselineRate / kMicroframesPerSec;
    ivars->fGainQ16 = kGainQ16Unity;
    ivars->fVolumeGainQ16 = kGainQ16Unity;

    // ---- device-level ownership (keeps usbaudiod off every interface) ----
    IOUSBHostDevice *device = OSDynamicCast(IOUSBHostDevice, provider);
    IOUSBHostInterface *iface = NULL;
    bool ifaceOpened = false;
    if (!device) {
        os_log(LOG, LOG_PREFIX "provider is not IOUSBHostDevice");
        return kIOReturnInvalid;
    }
    ret = device->Open(this, 0, NULL);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "device->Open failed (0x%x)", ret);
        return ret;
    }
    device->retain();
    ivars->fDevice = device;

    // Same-value SetConfiguration is a no-op, so drop to unconfigured first;
    // matchInterfaces=false keeps usbaudiod / HID drivers from claiming anything.
    ret = device->SetConfiguration(0, false);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "SetConfiguration(0) returned 0x%x (continuing)", ret);
    }
    ret = device->SetConfiguration(kConfigValue, false);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "SetConfiguration(%u) failed (0x%x)", kConfigValue, ret);
        goto fail_usb;
    }
    os_log(LOG, LOG_PREFIX "SetConfiguration(%u) OK", kConfigValue);

    {
        const IOUSBConfigurationDescriptor *cfg =
            device->CopyConfigurationDescriptorWithValue(kConfigValue);
        uintptr_t iter = 0;
        if (cfg && device->CreateInterfaceIterator(&iter) == kIOReturnSuccess) {
            IOUSBHostInterface *cand = NULL;
            while (device->CopyInterface(iter, &cand) == kIOReturnSuccess && cand) {
                const IOUSBInterfaceDescriptor *idesc = cand->GetInterfaceDescriptor(cfg);
                if (idesc) {
                    os_log(LOG, LOG_PREFIX "enumerated IF#%u class=%u sub=%u",
                           idesc->bInterfaceNumber, idesc->bInterfaceClass,
                           idesc->bInterfaceSubClass);
                }
                if (idesc && idesc->bInterfaceNumber == kStreamingIfNum) {
                    iface = cand;
                    break;
                }
                OSSafeReleaseNULL(cand);
            }
            device->DestroyInterfaceIterator(iter);
        }
        if (cfg) IOUSBHostFreeDescriptor(cfg);
    }
    if (!iface) {
        os_log(LOG, LOG_PREFIX "streaming IF#%u not found", kStreamingIfNum);
        ret = kIOReturnNotFound;
        goto fail_usb;
    }
    ivars->fInterface = iface;

    for (int attempt = 1; ; attempt++) {
        ret = iface->Open(this, 0, NULL);
        if (ret == kIOReturnSuccess) {
            os_log(LOG, LOG_PREFIX "iface->Open OK (attempt %d)", attempt);
            break;
        }
        if (attempt >= 20) {
            os_log(LOG, LOG_PREFIX "iface->Open failed (0x%x) after %d attempts", ret, attempt);
            goto fail_usb;
        }
        IOSleep(50);
    }
    ifaceOpened = true;

    // Linux order: alt 0 -> mode command -> clock -> streaming alt
    SelectAlt(iface, 0);
    SendPcmModeCommand(this, device);
    BuildRateList(this, device, ivars);
    if (!RateIsPublished(ivars, kBaselineRate)) {
        ivars->fRateHz = ivars->fRates[0];
        ivars->fFbCurrentQ16 = NominalFbQ16(ivars->fRateHz);
    }
    SendClockSampleRate(this, device, ivars->fRateHz);
    ret = SelectAlt(iface, kStreamingAlt);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "SelectAlternateSetting(%u) failed (0x%x)", kStreamingAlt, ret);
        goto fail_usb;
    }
    os_log(LOG, LOG_PREFIX "SelectAlternateSetting(%u) OK", kStreamingAlt);

    ret = iface->CopyPipe(kOutEP, &ivars->fOutPipe);
    if (ret != kIOReturnSuccess || !ivars->fOutPipe) {
        os_log(LOG, LOG_PREFIX "CopyPipe(OUT) failed (0x%x)", ret);
        goto fail_usb;
    }
    ret = iface->CopyPipe(kFbEP, &ivars->fFbPipe);
    if (ret != kIOReturnSuccess || !ivars->fFbPipe) {
        os_log(LOG, LOG_PREFIX "CopyPipe(FB) failed (0x%x)", ret);
        goto fail_usb;
    }

    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn, 4, 4, &ivars->fFbDataBuf);
    if (ret != kIOReturnSuccess) goto fail_usb;
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                           sizeof(IOUSBIsochronousFrame), 8, &ivars->fFbFrameList);
    if (ret != kIOReturnSuccess) goto fail_usb;
    ret = CreateActionFeedbackComplete(0, &ivars->fFbAction);
    if (ret != kIOReturnSuccess) goto fail_usb;

    for (uint32_t s = 0; s < kOutXfersInFlight; s++) {
        ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOut, kOutBufSize, 8,
                                               &ivars->fOutDataBufs[s]);
        if (ret != kIOReturnSuccess) goto fail_usb;
        ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                               kNumOutFrames * sizeof(IOUSBIsochronousFrame),
                                               8, &ivars->fOutFrameLists[s]);
        if (ret != kIOReturnSuccess) goto fail_usb;
    }
    ret = CreateActionOutComplete(0, &ivars->fOutAction);
    if (ret != kIOReturnSuccess) goto fail_usb;
    os_log(LOG, LOG_PREFIX "USB setup complete");

    // ---- AudioDriverKit objects ----
    {
        OSString *deviceUID   = OSString::withCString("jp.hogehoge.TeacAI501DA.UD-501V2.device");
        OSString *modelUID    = OSString::withCString("UD-501V2");
        // Shown as "Manufacturer" in System Information > Audio; names this
        // driver so the user can tell it from the stock one there.
        OSString *manufactUID = OSString::withCString("TEAC (TeacAI501DA driver)");

        TeacUD501Device *dev = OSTypeAlloc(TeacUD501Device);
        if (!dev || !dev->init(this, true, deviceUID, modelUID, manufactUID, kZtsPeriodFrames)) {
            os_log(LOG, LOG_PREFIX "TeacUD501Device init failed");
            OSSafeReleaseNULL(dev);
            OSSafeReleaseNULL(deviceUID);
            OSSafeReleaseNULL(modelUID);
            OSSafeReleaseNULL(manufactUID);
            ret = kIOReturnNoResources;
            goto fail_audio;
        }
        OSSafeReleaseNULL(deviceUID);
        OSSafeReleaseNULL(modelUID);
        OSSafeReleaseNULL(manufactUID);
        dev->SetDriverRef(this);
        ivars->fAudioDevice = dev;
        ivars->fAudioDevice->SetCanBeDefaultOutputDevice(true);
        ivars->fAudioDevice->SetCanBeDefaultSystemOutputDevice(true);
        {
            double rates[kNumCandidateRates];
            for (uint32_t i = 0; i < ivars->fNumRates; i++) {
                rates[i] = (double)ivars->fRates[i];
            }
            ivars->fAudioDevice->SetAvailableSampleRates(rates, ivars->fNumRates);
            ivars->fAudioDevice->SetSampleRate((double)ivars->fRateHz);
            ivars->fAudioDevice->SetTransportType(IOUserAudioTransportType::USB);
            ivars->fAudioDevice->SetOutputSafetyOffset(SafetyOffsetForRate(ivars->fRateHz));
        }

        ivars->fCoreAudioBufLen = (size_t)kZtsPeriodFrames * kRingBytesPerFrame;
        ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                               ivars->fCoreAudioBufLen, 64, &ivars->fCoreAudioBuf);
        if (ret != kIOReturnSuccess) goto fail_audio;
        {
            IOAddressSegment seg;
            if (ivars->fCoreAudioBuf->GetAddressRange(&seg) == kIOReturnSuccess) {
                ivars->fCoreAudioBufPtr = (void *)seg.address;
                memset(ivars->fCoreAudioBufPtr, 0, ivars->fCoreAudioBufLen);
            }
        }

        auto streamSp = IOUserAudioStream::Create(this,
                            IOUserAudioStreamDirection::Output, ivars->fCoreAudioBuf);
        if (!streamSp.get()) {
            ret = kIOReturnNoResources;
            goto fail_audio;
        }
        ivars->fOutputStream = streamSp.get();
        ivars->fOutputStream->retain();
        {
            IOUserAudioStreamBasicDescription fmts[kNumCandidateRates];
            for (uint32_t i = 0; i < ivars->fNumRates; i++) {
                fmts[i] = MakeFormat24((double)ivars->fRates[i]);
            }
            ivars->fOutputStream->SetAvailableStreamFormats(fmts, ivars->fNumRates);
            IOUserAudioStreamBasicDescription cur = MakeFormat24((double)ivars->fRateHz);
            ivars->fOutputStream->SetCurrentStreamFormat(&cur);
        }
        ivars->fAudioDevice->AddStream(ivars->fOutputStream);

        {
            // Same name the stock driver shows, so existing output-selection
            // scripts keep matching the unit. The manufacturer field (System
            // Information > Audio) is what tells this driver apart.
            OSString *devName = OSString::withCString("TEAC USB AUDIO DEVICE");
            if (devName) {
                ivars->fAudioDevice->SetName(devName);
                OSSafeReleaseNULL(devName);
            }
        }
        ret = AddObject(ivars->fAudioDevice);
        if (ret != kIOReturnSuccess) {
            os_log(LOG, LOG_PREFIX "AddObject(fAudioDevice) failed (0x%x)", ret);
            goto fail_audio;
        }

#if !TEAC_FIXED_VOLUME
        {
            IOUserAudioLevelControlRange range = { kVolumeMinDb, kVolumeMaxDb };
            TeacUD501VolumeControl *vol = OSTypeAlloc(TeacUD501VolumeControl);
            if (!vol || !vol->init(this, true, kVolumeMaxDb, range,
                                   IOUserAudioObjectPropertyElementMain,
                                   IOUserAudioObjectPropertyScope::Output,
                                   IOUserAudioClassID::VolumeControl)) {
                OSSafeReleaseNULL(vol);
                ret = kIOReturnNoResources;
                goto fail_audio;
            }
            vol->SetDriverRef(this);
            ivars->fVolumeControl = vol;
            ret = ivars->fAudioDevice->AddControl(vol);
            if (ret != kIOReturnSuccess) goto fail_audio;

            TeacUD501MuteControl *mute = OSTypeAlloc(TeacUD501MuteControl);
            if (!mute || !mute->init(this, true, false,
                                     IOUserAudioObjectPropertyElementMain,
                                     IOUserAudioObjectPropertyScope::Output,
                                     IOUserAudioClassID::MuteControl)) {
                OSSafeReleaseNULL(mute);
                ret = kIOReturnNoResources;
                goto fail_audio;
            }
            mute->SetDriverRef(this);
            ivars->fMuteControl = mute;
            ret = ivars->fAudioDevice->AddControl(mute);
            if (ret != kIOReturnSuccess) goto fail_audio;
            os_log(LOG, LOG_PREFIX "volume/mute controls registered (range %d..%d dB)",
                   (int)kVolumeMinDb, (int)kVolumeMaxDb);
        }
#else
        os_log(LOG, LOG_PREFIX "fixed-volume build — no volume/mute controls");
#endif

        ivars->fAudioDevice->SetIOOperationHandler(
            ^(IOUserAudioObjectID device,
              IOUserAudioIOOperation op,
              uint32_t frameSize,
              uint64_t sampleTime,
              uint64_t hostTime) {
                (void)device; (void)hostTime;
                TeacUD501_IVars *iv = ivars;
                if (!iv || iv->fStopping) {
                    return kIOReturnSuccess;
                }
                if (op == IOUserAudioIOOperationWriteEnd) {
                    iv->fCoreAudioWriteEndSample = sampleTime + frameSize;
                }
                return kIOReturnSuccess;
            });
    }

    ret = Ud501ArmUsbStreaming(ivars);
    if (ret != kIOReturnSuccess) {
        return ret;
    }
    ret = RegisterService();
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "RegisterService failed (0x%x)", ret);
        return ret;
    }
    os_log(LOG, LOG_PREFIX "armed at %u Hz", ivars->fRateHz);
    return kIOReturnSuccess;

fail_audio:
    OSSafeReleaseNULL(ivars->fVolumeControl);
    OSSafeReleaseNULL(ivars->fMuteControl);
    OSSafeReleaseNULL(ivars->fOutputStream);
    OSSafeReleaseNULL(ivars->fCoreAudioBuf);
    OSSafeReleaseNULL(ivars->fAudioDevice);
fail_usb:
    os_log(LOG, LOG_PREFIX "Start failed (0x%x)", ret);
    OSSafeReleaseNULL(ivars->fOutAction);
    for (uint32_t s = 0; s < kOutXfersInFlight; s++) {
        OSSafeReleaseNULL(ivars->fOutFrameLists[s]);
        OSSafeReleaseNULL(ivars->fOutDataBufs[s]);
    }
    OSSafeReleaseNULL(ivars->fFbAction);
    OSSafeReleaseNULL(ivars->fFbFrameList);
    OSSafeReleaseNULL(ivars->fFbDataBuf);
    OSSafeReleaseNULL(ivars->fFbPipe);
    OSSafeReleaseNULL(ivars->fOutPipe);
    if (ivars->fInterface) {
        if (ifaceOpened) ivars->fInterface->Close(this, 0);
        OSSafeReleaseNULL(ivars->fInterface);
    }
    if (ivars->fDevice) {
        ivars->fDevice->Close(this, 0);
        OSSafeReleaseNULL(ivars->fDevice);
    }
    return ret;
}

void
IMPL(TeacUD501, FeedbackComplete)
{
    Ud501IoToken _tok(ivars);
    if (!ivars || ivars->fStopping) {
        return;
    }
    if (status == kIOReturnSuccess) {
        IOAddressSegment seg;
        if (ivars->fFbDataBuf->GetAddressRange(&seg) == kIOReturnSuccess) {
            uint32_t raw = ReadLE32((const volatile void *)seg.address);
            // UAC2 high-speed feedback is Q16.16 samples per µframe. The real rate
            // differs from nominal by ppm, so a ±1.5% band separates the encodings:
            // raw as-is (spec), or raw + 1.0 (the AI-501DA's TE8802 reports
            // "actual − 1"). Anything else is rejected and pacing keeps the last
            // good value.
            uint32_t nom = NominalFbQ16(ivars->fRateHz);
            uint32_t band = (uint32_t)(((uint64_t)nom * 15) / 1000);
            uint32_t plus1 = raw + (1u << 16);
            uint32_t mode = 0;
            uint32_t fbQ16 = 0;
            if (raw > nom - band && raw < nom + band) {
                mode = 1; fbQ16 = raw;
            } else if (plus1 > nom - band && plus1 < nom + band) {
                mode = 2; fbQ16 = plus1;
            }
            if (mode) {
                if (mode != ivars->fFbMode) {
                    os_log(LOG, LOG_PREFIX "feedback encoding %{public}s (raw=0x%08x nom=0x%08x)",
                           mode == 1 ? "Q16.16 as-is" : "Q16.16 + 1.0", raw, nom);
                    ivars->fFbMode = mode;
                }
                ivars->fFbCurrentQ16 = fbQ16;
            } else {
                ivars->fFbRejected++;
            }
            ivars->fFbCount++;
            if ((ivars->fFbCount % 1000) == 1) {
                os_log(LOG, LOG_PREFIX "Fb #%llu raw=0x%08x cur=0x%08x nom=0x%08x rejected=%llu",
                       ivars->fFbCount, raw, ivars->fFbCurrentQ16, nom, ivars->fFbRejected);
            }
        }
    } else {
        os_log(LOG, LOG_PREFIX "FeedbackComplete error 0x%x", status);
    }
    if (status == kIOReturnAborted || status == kIOReturnNotResponding ||
        status == kIOReturnNoDevice) {
        return;
    }
    if (!ivars->fReconfiguring && ivars->fFbPipe && ivars->fFbAction) {
        IOAddressSegment flSeg;
        if (ivars->fFbFrameList->GetAddressRange(&flSeg) == kIOReturnSuccess) {
            IOUSBIsochronousFrame *f = (IOUSBIsochronousFrame *)flSeg.address;
            f->status = kIOReturnSuccess; f->requestCount = 4;
            f->completeCount = 0; f->timeStamp = 0;
        }
        IoInc(ivars);
        kern_return_t r = ivars->fFbPipe->IsochIO(ivars->fFbDataBuf, ivars->fFbFrameList,
                                                  0, ivars->fFbAction);
        if (r != kIOReturnSuccess) {
            IoDec(ivars);
            os_log(LOG, LOG_PREFIX "Fb re-IsochIO failed (0x%x)", r);
        }
    }
}

void
IMPL(TeacUD501, OutComplete)
{
    Ud501IoToken _tok(ivars);
    if (!ivars || ivars->fStopping) {
        return;
    }
    if (status != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "OutComplete error 0x%x (#%llu)", status, ivars->fOutCount);
        if (status == kIOReturnAborted || status == kIOReturnNotResponding ||
            status == kIOReturnNoDevice) {
            return;
        }
    } else {
        ivars->fOutCount++;
        if ((ivars->fOutCount % 1000) == 1) {
            uint64_t diverg = (ivars->fUsbCumulativeSamples >= ivars->fUsbCompletedSamples)
                            ? ivars->fUsbCumulativeSamples - ivars->fUsbCompletedSamples : 0;
            os_log(LOG, LOG_PREFIX "OUT #%llu rate=%u samples/uF=%u usbCumul=%llu caEnd=%llu bad=%llu short=%llu peak=%u lastBad=0x%08x deficitSmp=%llu diverg=%llu maxDiverg=%llu overrun=%llu resyncN=%llu reanchor=%llu silence=%llu dop=%llu gainQ16=%u",
                   ivars->fOutCount, ivars->fRateHz, ivars->fSamplesPerUF,
                   ivars->fUsbCumulativeSamples, ivars->fCoreAudioWriteEndSample,
                   ivars->fOutBadFrames, ivars->fOutShortFrames, ivars->fPeakLevel,
                   ivars->fLastBadStatus, ivars->fShortDeficitSamples, diverg,
                   ivars->fMaxDivergence, ivars->fRingOverrunEvents, ivars->fResyncCount,
                   ivars->fRingGuardReanchors, ivars->fSilenceFilledSamples,
                   ivars->fDopChunks, ivars->fGainQ16);
            ivars->fPeakLevel = 0;
        }
    }

    if (ivars->fReconfiguring || !ivars->fOutPipe || !ivars->fOutAction) {
        return;
    }

    uint32_t slot = ivars->fOutSlot;
    ivars->fOutSlot = (slot + 1) % kOutXfersInFlight;
    IOBufferMemoryDescriptor *dataBuf   = ivars->fOutDataBufs[slot];
    IOBufferMemoryDescriptor *frameList = ivars->fOutFrameLists[slot];

    // 0) completed frame statuses -> ZTS clock axis
    IOAddressSegment flSeg = {};
    uint32_t totalSamplesNext = 0;
    if (frameList->GetAddressRange(&flSeg) == kIOReturnSuccess) {
        IOUSBIsochronousFrame *frames = (IOUSBIsochronousFrame *)flSeg.address;
        uint32_t completedBytes = 0;
        for (uint32_t i = 0; i < kNumOutFrames; i++) {
            IOReturn st = frames[i].status;
            if (st != kIOReturnSuccess) {
                ivars->fOutBadFrames++;
                ivars->fLastBadStatus = (uint32_t)st;
                if (!ivars->fFirstBadLogged) {
                    os_log(LOG, LOG_PREFIX "first bad frame status 0x%08x at #%llu frame %u req=%u done=%u rate=%u",
                           (uint32_t)st, ivars->fOutCount, i,
                           frames[i].requestCount, frames[i].completeCount, ivars->fRateHz);
                    ivars->fFirstBadLogged = true;
                }
            } else if (frames[i].completeCount < frames[i].requestCount) {
                ivars->fOutShortFrames++;
                ivars->fShortDeficitSamples +=
                    (frames[i].requestCount - frames[i].completeCount) / kWireBytesPerFrame;
            }
            completedBytes += frames[i].completeCount;
        }
        ivars->fUsbCompletedSamples += completedBytes / kWireBytesPerFrame;

        // 1) next frame sizes from feedback pacing
        FillOutFrameList(frames, kNumOutFrames, ivars);
        for (uint32_t i = 0; i < kNumOutFrames; i++) {
            totalSamplesNext += frames[i].requestCount / kWireBytesPerFrame;
        }
    }

    // 2) CoreAudio ring (packed 24-bit) -> USB OUT (24-bit MSB-aligned in 4-byte slots)
    if (flSeg.address && ivars->fCoreAudioBufPtr && dataBuf) {
        IOAddressSegment outSeg;
        if (dataBuf->GetAddressRange(&outSeg) == kIOReturnSuccess) {
            uint8_t *out = (uint8_t *)outSeg.address;
            const uint8_t *src = (const uint8_t *)ivars->fCoreAudioBufPtr;
            const uint64_t ringFrames = kZtsPeriodFrames;

            // reader re-anchor when about to overtake the writer (AI-501DA v25)
            uint64_t writePos = ivars->fCoreAudioWriteEndSample;
            bool playing = (writePos != ivars->fPrevWriteEnd);
            if (playing && (ivars->fUsbCumulativeSamples + totalSamplesNext) > writePos) {
                uint32_t safety = SafetyOffsetForRate(ivars->fRateHz);
                uint64_t safeStart = (writePos > (uint64_t)safety) ? (writePos - safety) : 0;
                if (safeStart < ivars->fUsbCumulativeSamples) {
                    ivars->fUsbCumulativeSamples = safeStart;
                    ivars->fRingGuardReanchors++;
                }
            }
            uint64_t startSample = ivars->fUsbCumulativeSamples;

            // silence fill beyond what CoreAudio has written (AI-501DA v30)
            uint32_t availFrames = totalSamplesNext;
            if (writePos <= startSample) {
                availFrames = 0;
            } else if ((writePos - startSample) < (uint64_t)totalSamplesNext) {
                availFrames = (uint32_t)(writePos - startSample);
            }
            if (availFrames < totalSamplesNext) {
                ivars->fSilenceFilledSamples += totalSamplesNext - availFrames;
                if (!ivars->fUnderrunActive && writePos != 0) {
                    ivars->fUnderrunActive = true;
                    os_log(LOG, LOG_PREFIX "supply underrun — silence fill engaged (read=%llu write=%llu)",
                           startSample, writePos);
                }
            } else if (ivars->fUnderrunActive) {
                ivars->fUnderrunActive = false;
                os_log(LOG, LOG_PREFIX "supply resumed — silence fill disengaged (read=%llu write=%llu)",
                       startSample, writePos);
            }

            uint32_t nFrames = totalSamplesNext;
            if ((uint64_t)nFrames * kWireBytesPerFrame > kOutBufSize) {
                nFrames = kOutBufSize / kWireBytesPerFrame;   // cannot happen with sane feedback
            }
            for (uint32_t f = 0; f < nFrames; f++) {
                uint8_t *o = out + (size_t)f * kWireBytesPerFrame;
                if (f < availFrames) {
                    const uint8_t *s = src + (size_t)((startSample + f) % ringFrames) * kRingBytesPerFrame;
                    o[0] = 0; o[1] = s[0]; o[2] = s[1]; o[3] = s[2];
                    o[4] = 0; o[5] = s[3]; o[6] = s[4]; o[7] = s[5];
                } else {
                    memset(o, 0, kWireBytesPerFrame);
                }
            }

#if !TEAC_FIXED_VOLUME
            // software volume; unity skips the loop (bit-perfect). DoP chunks bypass.
            uint32_t gain = __atomic_load_n(&ivars->fGainQ16, __ATOMIC_ACQUIRE);
            if (gain != kGainQ16Unity && nFrames > 0) {
                if (ChunkLooksLikeDoP(out, nFrames)) {
                    ivars->fDopChunks++;
                } else if (gain == 0) {
                    memset(out, 0, (size_t)nFrames * kWireBytesPerFrame);
                } else {
                    // 24-bit in, 32-bit out: the attenuated value keeps 8 extra
                    // bits below the 24-bit LSB instead of truncating.
                    for (uint32_t i = 0; i < nFrames * 2; i++) {
                        uint8_t *o = out + (size_t)i * 4;
                        int32_t v = (int32_t)(((uint32_t)o[1] << 8) | ((uint32_t)o[2] << 16) |
                                              ((uint32_t)o[3] << 24)) >> 8;   // sign-extended 24-bit
                        int32_t w = (int32_t)(((int64_t)v * (int64_t)gain) >> 8);
                        o[0] = (uint8_t)(w & 0xFF);
                        o[1] = (uint8_t)((w >> 8) & 0xFF);
                        o[2] = (uint8_t)((w >> 16) & 0xFF);
                        o[3] = (uint8_t)((w >> 24) & 0xFF);
                    }
                }
            }
#endif
            // peak (top 16 bits of each slot) and a once-a-second MSB dump (DoP check)
            for (uint32_t i = 0; i < nFrames * 2; i++) {
                const uint8_t *o = out + (size_t)i * 4;
                int16_t v = (int16_t)((uint16_t)o[2] | ((uint16_t)o[3] << 8));
                uint32_t a = (v < 0) ? (uint32_t)(-(int32_t)v) : (uint32_t)v;
                if (a > ivars->fPeakLevel) ivars->fPeakLevel = a;
            }
            if ((ivars->fOutCount % 1000) == 1 && nFrames >= 8) {
                os_log(LOG, LOG_PREFIX "MSB8L= %02x %02x %02x %02x %02x %02x %02x %02x",
                       out[3], out[11], out[19], out[27], out[35], out[43], out[51], out[59]);
            }
            ivars->fUsbCumulativeSamples += totalSamplesNext;

            uint64_t diverg = (ivars->fUsbCumulativeSamples >= ivars->fUsbCompletedSamples)
                            ? (ivars->fUsbCumulativeSamples - ivars->fUsbCompletedSamples) : 0;
            if (diverg > ivars->fMaxDivergence) ivars->fMaxDivergence = diverg;
            bool writing = (ivars->fCoreAudioWriteEndSample != ivars->fPrevWriteEnd);
            ivars->fPrevWriteEnd = ivars->fCoreAudioWriteEndSample;
            if (writing && ivars->fCoreAudioWriteEndSample < ivars->fUsbCumulativeSamples) {
                ivars->fRingOverrunEvents++;
            }
        }
    }

    // 3) zero timestamp from completed samples, once per period
    if (ivars->fAudioDevice && ivars->fUsbCompletedSamples >= ivars->fNextZtsSample) {
        ivars->fAudioDevice->UpdateCurrentZeroTimestamp(ivars->fNextZtsSample, mach_absolute_time());
        ivars->fNextZtsSample += kZtsPeriodFrames;
    }

    // 4) re-enqueue at the next consecutive frame number
    IoInc(ivars);
    kern_return_t r = ivars->fOutPipe->IsochIO(dataBuf, frameList,
                                               ivars->fOutFrameNumber, ivars->fOutAction);
    if (r == kIOReturnSuccess) {
        ivars->fOutFrameNumber++;
    } else {
        IoDec(ivars);
        uint64_t f = 0;
        if (ivars->fDevice && ivars->fDevice->GetFrameNumber(&f, NULL) == kIOReturnSuccess) {
            ivars->fResyncCount++;
            os_log(LOG, LOG_PREFIX "OUT re-IsochIO 0x%x @%llu — resync to %llu (resyncN=%llu)",
                   r, ivars->fOutFrameNumber, f + 4, ivars->fResyncCount);
            ivars->fOutFrameNumber = f + 4;
            IoInc(ivars);
            r = ivars->fOutPipe->IsochIO(dataBuf, frameList, ivars->fOutFrameNumber, ivars->fOutAction);
            if (r == kIOReturnSuccess) {
                ivars->fOutFrameNumber++;
            } else {
                IoDec(ivars);
                os_log(LOG, LOG_PREFIX "OUT resync IsochIO failed (0x%x)", r);
            }
        }
    }
}

kern_return_t
TeacUD501::ReconfigureForRate(uint32_t in_rate_hz)
{
    if (!ivars || !ivars->fInterface || !ivars->fDevice ||
        !ivars->fOutPipe || !ivars->fFbPipe) {
        return kIOReturnNotReady;
    }
    if (!RateIsPublished(ivars, in_rate_hz)) {
        os_log(LOG, LOG_PREFIX "ReconfigureForRate: unsupported %u", in_rate_hz);
        return kIOReturnUnsupported;
    }
    if (in_rate_hz == ivars->fRateHz && !ivars->fReconfiguring) {
        return kIOReturnSuccess;
    }

    Ud501ReconfToken _reconf(&ivars->fInReconfigure);
    if (__atomic_load_n(&ivars->fStopping, __ATOMIC_SEQ_CST)) {
        return kIOReturnOffline;
    }
    os_log(LOG, LOG_PREFIX "ReconfigureForRate %u -> %u", ivars->fRateHz, in_rate_hz);

    // Abort completions queue up behind us on this same default queue; the
    // re-arm happens when the last one drains (AI-501DA v29).
    ivars->fReconfiguring = true;
    ivars->fOutPipe->Abort(0, kIOReturnAborted, NULL);
    ivars->fFbPipe->Abort(0, kIOReturnAborted, NULL);

    SelectAlt(ivars->fInterface, 0);
    SendPcmModeCommand(this, ivars->fDevice);
    kern_return_t ret = SendClockSampleRate(this, ivars->fDevice, in_rate_hz);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "clock SET_CUR %u failed (0x%x) — restoring %u",
               in_rate_hz, ret, ivars->fRateHz);
        SendClockSampleRate(this, ivars->fDevice, ivars->fRateHz);
        in_rate_hz = ivars->fRateHz;
    }
    kern_return_t altRet = SelectAlt(ivars->fInterface, kStreamingAlt);
    if (altRet != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "SelectAlternateSetting(%u) failed (0x%x)", kStreamingAlt, altRet);
        return altRet;   // fReconfiguring stays set; the next switch retries in full
    }

    // alt cycling recreates the endpoints — re-copy the pipes
    OSSafeReleaseNULL(ivars->fOutPipe);
    OSSafeReleaseNULL(ivars->fFbPipe);
    kern_return_t pr = ivars->fInterface->CopyPipe(kOutEP, &ivars->fOutPipe);
    if (pr != kIOReturnSuccess || !ivars->fOutPipe) {
        os_log(LOG, LOG_PREFIX "re-CopyPipe(OUT) failed (0x%x)", pr);
        return pr ? pr : kIOReturnNotFound;
    }
    pr = ivars->fInterface->CopyPipe(kFbEP, &ivars->fFbPipe);
    if (pr != kIOReturnSuccess || !ivars->fFbPipe) {
        os_log(LOG, LOG_PREFIX "re-CopyPipe(FB) failed (0x%x)", pr);
        return pr ? pr : kIOReturnNotFound;
    }

    ivars->fRateHz = in_rate_hz;
    ivars->fFbCurrentQ16 = NominalFbQ16(in_rate_hz);
    ivars->fFbAccumQ16 = 0;
    ivars->fFbMode = 0;
    ivars->fUsbCumulativeSamples = 0;
    ivars->fUsbCompletedSamples = 0;
    ivars->fCoreAudioWriteEndSample = 0;
    ivars->fPrevWriteEnd = 0;
    ivars->fOutBadFrames = 0;
    ivars->fOutShortFrames = 0;
    ivars->fShortDeficitSamples = 0;
    ivars->fLastBadStatus = 0;
    ivars->fFirstBadLogged = false;
    ivars->fPeakLevel = 0;
    ivars->fSilenceFilledSamples = 0;
    ivars->fUnderrunActive = false;
    ivars->fNextZtsSample = kZtsPeriodFrames;
    if (ivars->fCoreAudioBufPtr) {
        memset(ivars->fCoreAudioBufPtr, 0, ivars->fCoreAudioBufLen);
    }
    if (ivars->fOutputStream) {
        IOUserAudioStreamBasicDescription fmt = MakeFormat24((double)in_rate_hz);
        ivars->fOutputStream->SetCurrentStreamFormat(&fmt);
    }
    if (ivars->fAudioDevice) {
        ivars->fAudioDevice->SetOutputSafetyOffset(SafetyOffsetForRate(in_rate_hz));
        ivars->fAudioDevice->UpdateCurrentZeroTimestamp(0, mach_absolute_time());
    }

    kern_return_t armRet = Ud501ScheduleRearmAfterDrain(ivars);
    os_log(LOG, LOG_PREFIX "rate %u armed (ret=0x%x deferred=%d)", in_rate_hz, armRet,
           ivars->fRearmAfterDrain ? 1 : 0);
    return (ret != kIOReturnSuccess) ? ret : armRet;
}

void
TeacUD501::UpdateOutputVolume(float in_decibel)
{
    if (!ivars) {
        return;
    }
    uint32_t gain = GainQ16FromDecibel(in_decibel);
    __atomic_store_n(&ivars->fVolumeGainQ16, gain, __ATOMIC_RELAXED);
    uint32_t eff = ivars->fOutputMuted ? 0 : gain;
    __atomic_store_n(&ivars->fGainQ16, eff, __ATOMIC_RELEASE);
    os_log(LOG, LOG_PREFIX "volume -> %d.%u dB gainQ16=%u muted=%d",
           (int)in_decibel,
           (uint32_t)((in_decibel - (int)in_decibel) * -10.0f + 0.5f) % 10,
           gain, ivars->fOutputMuted ? 1 : 0);
}

void
TeacUD501::UpdateOutputMute(bool in_muted)
{
    if (!ivars) {
        return;
    }
    ivars->fOutputMuted = in_muted;
    uint32_t eff = in_muted ? 0 : __atomic_load_n(&ivars->fVolumeGainQ16, __ATOMIC_RELAXED);
    __atomic_store_n(&ivars->fGainQ16, eff, __ATOMIC_RELEASE);
    os_log(LOG, LOG_PREFIX "mute -> %d gainQ16=%u", in_muted ? 1 : 0, eff);
}

kern_return_t
IMPL(TeacUD501, Stop)
{
    // Quiesce exactly like the AI-501DA class (v28): fStopping -> alt 0 + Close
    // (aborts pipe IO) -> OSAction::Cancel -> release + super Stop in the handler.
    if (!ivars) {
        return Stop(provider, SUPERDISPATCH);
    }
    os_log(LOG, LOG_PREFIX "Stop: fb=%llu out=%llu inflight=%u — quiescing",
           ivars->fFbCount, ivars->fOutCount,
           __atomic_load_n(&ivars->fIoInFlight, __ATOMIC_ACQUIRE));
    __atomic_store_n(&ivars->fStopping, true, __ATOMIC_SEQ_CST);

    if (ivars->fInterface) {
        ivars->fInterface->SelectAlternateSetting(0);
        ivars->fInterface->Close(this, 0);
    }
    if (ivars->fDevice) {
        ivars->fDevice->Close(this, 0);
    }

    void (^finish)(void) = ^{
        uint32_t waitedMs = 0;
        while (__atomic_load_n(&ivars->fInReconfigure, __ATOMIC_SEQ_CST) != 0) {
            IOSleep(5);
            waitedMs += 5;
            if ((waitedMs % 5000) == 0) {
                os_log(LOG, LOG_PREFIX "Stop: still waiting for reconfigure (%ums)", waitedMs);
            }
        }
        os_log(LOG, LOG_PREFIX "Stop: teardown (reconf-wait %ums)", waitedMs);
        OSSafeReleaseNULL(ivars->fVolumeControl);
        OSSafeReleaseNULL(ivars->fMuteControl);
        OSSafeReleaseNULL(ivars->fOutputStream);
        OSSafeReleaseNULL(ivars->fCoreAudioBuf);
        OSSafeReleaseNULL(ivars->fAudioDevice);
        OSSafeReleaseNULL(ivars->fOutAction);
        for (uint32_t s = 0; s < kOutXfersInFlight; s++) {
            OSSafeReleaseNULL(ivars->fOutFrameLists[s]);
            OSSafeReleaseNULL(ivars->fOutDataBufs[s]);
        }
        OSSafeReleaseNULL(ivars->fFbAction);
        OSSafeReleaseNULL(ivars->fFbFrameList);
        OSSafeReleaseNULL(ivars->fFbDataBuf);
        OSSafeReleaseNULL(ivars->fOutPipe);
        OSSafeReleaseNULL(ivars->fFbPipe);
        OSSafeReleaseNULL(ivars->fInterface);
        OSSafeReleaseNULL(ivars->fDevice);
        Stop(provider, SUPERDISPATCH);
    };

    OSAction *outAction = ivars->fOutAction;
    OSAction *fbAction  = ivars->fFbAction;
    kern_return_t cr;
    if (outAction && fbAction) {
        cr = outAction->Cancel(^{
            kern_return_t cr2 = fbAction->Cancel(finish);
            if (cr2 != kIOReturnSuccess) {
                finish();
            }
        });
        if (cr != kIOReturnSuccess) {
            finish();
        }
        return kIOReturnSuccess;
    }
    if (outAction || fbAction) {
        OSAction *act = outAction ? outAction : fbAction;
        cr = act->Cancel(finish);
        if (cr != kIOReturnSuccess) {
            finish();
        }
        return kIOReturnSuccess;
    }
    finish();
    return kIOReturnSuccess;
}
