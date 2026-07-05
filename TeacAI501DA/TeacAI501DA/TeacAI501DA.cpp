//
//  TeacAI501DA.cpp
//  TeacAI501DA
//
//  Phase 3a: IOUserAudioDriver-derived dext that
//    (1) owns the whole AI-501DA device (v4: IOUSBHostDevice match) and opens IF#3
//    (2) creates IOUserAudioDevice + ClockDevice + OutputStream (CoreAudio skeleton)
//    (3) Phase 3b will wire CoreAudio data into fOutDataBuf via SetIOOperationHandler.
//
//  v4: device-level ownership. v3 matched IF#3 alone and lost the boot-time Open
//  race against usbaudiod (iface->Open = 0xe00002cd), and could not issue the UAC2
//  clock SET_CUR (control interface not ours). SetConfiguration(matchInterfaces=false)
//  keeps usbaudiod away from every interface of this device.
//
//  GFEC ASSP8802 slow-PLL feedback correction is retained from Phase 2c-v2 because the
//  USB pacing problem exists independent of CoreAudio plumbing.
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

#include "TeacAI501DA.h"
#include "TeacAI501DADevice.h"

#define LOG OS_LOG_DEFAULT
#define LOG_PREFIX "TeacAI501DA: "

// Stream params for alt 1 (PCM 16bit / 2ch @ 48 kHz)
#define NUM_OUT_FRAMES                8
#define OUT_BYTES_PER_FRAME           156      // v21: alt2 (24bit) wMaxPacketSize
#define OUT_BUF_SIZE                  (NUM_OUT_FRAMES * OUT_BYTES_PER_FRAME)
// v11: transfers queued ahead on explicit consecutive frame numbers. A single
// re-armed transfer loses ~4% of frames to ASAP rescheduling (audible dropouts).
#define OUT_XFERS_IN_FLIGHT           4
#define OUT_BYTES_PER_SAMPLE          6        // v21: 24bit * 2ch (alt2)
#define STREAMING_ALT_SETTING         2        // v21: alt2 only — alt1 (16bit) caps at 88.2k
#define BASELINE_SAMPLE_RATE          48000U
#define MICROFRAMES_PER_SEC           8000U
#define BASELINE_SAMPLES_PER_UF       (BASELINE_SAMPLE_RATE / MICROFRAMES_PER_SEC)

// v17: supported nominal rates (alt 1 = 16bit; 192k needs 24 samples × 4B =
// 96B/µframe, within wMaxPacketSize 104)
static const uint32_t kSupportedRates[] = {44100, 48000, 88200, 96000, 176400, 192000};
#define NUM_SUPPORTED_RATES           (sizeof(kSupportedRates)/sizeof(kSupportedRates[0]))

// v16: the ZTS period MUST equal the ring buffer length in frames (maudio
// project's measured finding — mismatch causes a HAL read/write phase race
// that chops audio while all USB metrics stay healthy).
// v22: enlarged 16384 -> 49152 so hi-res rates keep a usable ring time window.
// At 16384 frames the ring was only 93 ms @176.4k / 85 ms @192k, too little
// margin -> the USB reader overtook CoreAudio's writes (~5% short frames).
// 49152 frames = 256 ms @192k / 1.024 s @48k. Ring bytes scale with this.
#define ZTS_PERIOD_FRAMES             49152    // ZeroTimestamp period == ring frames
// mach_absolute_time on Apple Silicon runs at 24 MHz → exactly 500 ticks per
// sample @48 kHz. Used to interpolate the host time of a ZTS boundary crossing.
#define MACH_TICKS_PER_SAMPLE_48K     500ULL

// v22: output safety offset scaled to the sample rate (~40 ms of head room).
// A fixed 1024 frames was 21 ms @48k but only 5.8 ms @176.4k — the HAL could
// not stay far enough ahead of the USB reader at hi-res, causing underruns.
static inline uint32_t
SafetyOffsetForRate(uint32_t rate_hz)
{
    uint32_t off = (uint32_t)(((uint64_t)rate_hz * 40) / 1000);  // 40 ms
    return off < 1024 ? 1024 : off;
}

// v13 experiment: ClockSource 12 is "Internal programmable Clock (synced to
// SOF)" — the DAC's audio clock is locked to USB SOF, i.e. it consumes exactly
// 48000.000 samples/s long-term. The ported ASSP8802 PLL converged to ~47998/s
// (−2 samples/s) and with lockDelay=2 the FIFO has no slack → periodic
// underruns (choppy audio). Send a constant 6 samples/µframe instead; feedback
// is still read and logged for observation.
#define ENABLE_ASSP8802_PLL           0

// v27: pace OUT by the DAC's explicit feedback (EP 0x84) instead of a fixed
// rate/8000. usbaudiod follows feedback and CAN lock DoP; our fixed pacing never
// established DoP lock (verified: dext delivers perfect 05/FA markers but the DAC
// stays muted). The AI-501DA (alt2) reports "actual consumption − 1", so the true
// per-µframe rate is Ff + 1.0 in Q16.16. Following it keeps the DAC's FIFO in its
// lock window (needed for DoP) and cancels long-term host/DAC drift (PCM robustness).
#define ENABLE_FEEDBACK_PACING        1

// nominal per-µframe sample count for a rate, in Q16.16 (used as the pre-feedback
// seed and to sanity-band incoming feedback).
static inline uint32_t
NominalFbQ16(uint32_t rate_hz)
{
    return (uint32_t)(((uint64_t)rate_hz << 16) / MICROFRAMES_PER_SEC);
}

// AI-501DA topology (ai501da_lsusb.txt)
#define DEVICE_CONFIG_VALUE           1
#define AUDIO_CONTROL_IF_NUM          2        // UAC2 AudioControl interface
#define STREAMING_IF_NUM              3        // UAC2 AudioStreaming interface
#define CLOCK_SOURCE_ID               12       // UAC2 ClockSource entity ID
#define UAC2_CUR                      0x01     // UAC2 CUR request
#define UAC2_CS_SAM_FREQ_CONTROL      0x01     // Clock Source control selector

static inline uint32_t
ReadLE32(const volatile void *p)
{
    const volatile uint8_t *b = (const volatile uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

struct TeacAI501DA_IVars
{
    // USB resources
    IOUSBHostDevice          *fDevice;
    IOUSBHostInterface       *fInterface;
    IOUSBHostPipe            *fFbPipe;
    IOUSBHostPipe            *fOutPipe;
    IOBufferMemoryDescriptor *fFbDataBuf;
    IOBufferMemoryDescriptor *fFbFrameList;
    OSAction                 *fFbAction;
    IOBufferMemoryDescriptor *fOutDataBufs[OUT_XFERS_IN_FLIGHT];
    IOBufferMemoryDescriptor *fOutFrameLists[OUT_XFERS_IN_FLIGHT];
    OSAction                 *fOutAction;
    uint64_t                  fOutFrameNumber;  // next USB frame number to schedule
    uint32_t                  fOutSlot;         // round-robin completion slot

    // ASSP8802 slow-PLL state
    uint32_t                  m_ulAvgSampleCnt;
    int32_t                   m_RunningFbFraction;
    int32_t                   m_RunningFbWhole;
    uint32_t                  fSamplesPerUF;

    // v17: multi-rate pacing (exact fractional accumulator, e.g. 44.1k = 5.5125/µframe)
    uint32_t                  fRateHz;          // current nominal rate
    uint32_t                  fRateAccum;       // running remainder (< MICROFRAMES_PER_SEC)
    bool                      fReconfiguring;   // rate switch in progress — don't re-arm IO
    // v27: explicit-feedback pacing (Q16.16 samples/µframe = decoded Ff + 1.0)
    uint32_t                  fFbCurrentQ16;    // latest DAC-consumption rate, seeded to nominal
    uint32_t                  fFbAccumQ16;      // fractional accumulator for whole-sample emission

    // CoreAudio (AudioDriverKit) objects — held as raw retained pointers
    IOUserAudioDevice        *fAudioDevice;     // retain()/release(); also the clock device
    IOUserAudioStream        *fOutputStream;
    IOBufferMemoryDescriptor *fCoreAudioBuf;
    void                     *fCoreAudioBufPtr; // cached virtual address
    size_t                    fCoreAudioBufLen;

    // USB <-> CoreAudio bridging state
    uint64_t                  fUsbCumulativeSamples;   // samples queued to USB OUT (ring read position)
    uint64_t                  fUsbCompletedSamples;    // samples actually completed on the wire (ZTS clock)
    uint64_t                  fCoreAudioWriteEndSample;// latest WriteEnd notification
    uint64_t                  fNextZtsSample;          // next ZTS boundary (multiple of ZTS_PERIOD_FRAMES)

    // debug counters
    uint64_t                  fFbCount;
    uint64_t                  fOutCount;
    uint64_t                  fOutBadFrames;    // per-frame status != success
    uint64_t                  fOutShortFrames;  // completeCount < requestCount
    uint32_t                  fPeakLevel;       // max |int16| copied since last log

    // v23: frame-status diagnostics. The aggregate bad/short counters revealed
    // that hi-res (176.4k/192k) accumulates ~46 bad + ~411 short frames/s, but
    // not *which* kIOReturn code the frames fail with, nor how many samples the
    // short frames actually drop. These pin down the failure mode.
    uint32_t                  fLastBadStatus;       // most recent frames[i].status != success
    bool                      fFirstBadLogged;      // one-shot: log first bad status with full context
    uint64_t                  fShortDeficitSamples; // cumulative (requestCount-completeCount) as samples
    uint32_t                  fWorstShortSamples;   // largest single-frame shortfall since last summary

    // v24 (Step 0, observation only — behavior unchanged): instrumentation to prove cause A/B.
    //   read axis  = fUsbCumulativeSamples (scheduled, incremented in full at L856)
    //   clock axis = fUsbCompletedSamples  (completed, used by ZTS)
    //   write axis = fCoreAudioWriteEndSample (end of what CoreAudio has written)
    // cause A: with shorts, (read - completed) = deficit sum grows monotonically until
    //          read overtakes write and reads the unwritten stale ring (= DoP breakdown).
    uint64_t                  fMaxDivergence;      // max(scheduled - completed), session max (kept across rate switches)
    uint64_t                  fRingOverrunEvents;  // completions where read overtook write during playback (evidence of cause A)
    uint64_t                  fPrevWriteEnd;       // used to detect active playback (= write advancing)
    uint64_t                  fResyncCount;        // resyncs after firstFrameNumber fell into the past (evidence of cause B)

    // v25 (cause A guard): count of reader re-anchors to just below write when it was
    // about to overtake. >0 = stale reads (= loud bursts) prevented. 0 is ideal (fixing B removes the trigger).
    uint64_t                  fRingGuardReanchors;

    // v28 (teardown/reconfigure quiesce): the old Stop() released everything and
    // completed IOSafeDeleteNULL(ivars) before the abort completions queued behind it
    // on the default queue were delivered -> the next OutComplete deref'd NULL ivars
    // (real crash: EXC_BAD_ACCESS 0x100, during the disable operation on 2026-07-04 20:50).
    //   fStopping     — after Stop begins, turns already-delivered handlers into no-ops (same queue, so no races)
    //   fIoInFlight   — count of outstanding isoch IOs + handler bodies still running (decremented via RAII).
    //                   v29: also drives the deferred re-arm on rate switches (drain detection).
    //                   NOTE: +1s from completions whose delivery Cancel dropped remain —
    //                   never wait for this counter to reach 0 after Stop
    //   fInReconfigure — rate-switch-in-progress flag. Measurements show switches also run
    //                   on the default queue, i.e. mutually exclusive with Stop automatically,
    //                   but the queue placement is undocumented, so the Dekker pair
    //                   (finish waits for 0) is kept as cross-queue insurance
    volatile bool             fStopping;
    volatile uint32_t         fIoInFlight;
    volatile uint32_t         fInReconfigure;
    // v29 measured (2026-07-04 23:09): ReconfigureForRate runs on the same default
    // queue as completion delivery (a quiesce sleep-wait always timed out, and all
    // completions were delivered in one burst right after return). Same queue =
    // mutual exclusion with handlers is automatic, but never sleep-wait for
    // completions on the queue. The re-arm is scheduled for the moment the last
    // outstanding completion drains (the IoInFlightToken dtor is the trigger).
    volatile bool             fRearmAfterDrain;
};

// v28: exactly-once accounting of isoch completions. +1 right before enqueueing an
// IsochIO (on failure no completion will come, so -1 immediately); the completion
// handler does -1 once its body has fully finished (RAII). Therefore
// fIoInFlight == 0 means "no outstanding IO and no handler currently running".
static inline void
IoInFlightInc(TeacAI501DA_IVars *iv)
{
    __atomic_add_fetch(&iv->fIoInFlight, 1, __ATOMIC_ACQ_REL);
}

static inline void
IoInFlightDec(TeacAI501DA_IVars *iv)
{
    __atomic_sub_fetch(&iv->fIoInFlight, 1, __ATOMIC_ACQ_REL);
}

static kern_return_t ArmUsbStreaming(TeacAI501DA_IVars *iv);

struct IoInFlightToken {
    TeacAI501DA_IVars *iv;
    explicit IoInFlightToken(TeacAI501DA_IVars *in_iv) : iv(in_iv) {}
    ~IoInFlightToken() {
        if (!iv) {
            return;
        }
        // v29: deferred re-arm scheduled by a rate switch — start IO at the new
        // rate once the last outstanding completion has drained (same queue, no races)
        if (__atomic_sub_fetch(&iv->fIoInFlight, 1, __ATOMIC_ACQ_REL) == 0 &&
            iv->fRearmAfterDrain && !iv->fStopping) {
            iv->fRearmAfterDrain = false;
            iv->fReconfiguring = false;
            kern_return_t r = ArmUsbStreaming(iv);
            os_log(LOG, LOG_PREFIX "drain complete — deferred re-arm (ret=0x%x)", r);
        }
    }
};

// v29: re-arm immediately if no completions are outstanding; otherwise schedule it
// for when the last one drains. ReconfigureForRate and completion delivery share the
// same default queue, so no handler runs while this function executes (confirmed by
// measurement — a sleep-wait can never be satisfied on the same queue).
static kern_return_t
ScheduleRearmAfterDrain(TeacAI501DA_IVars *iv)
{
    if (__atomic_load_n(&iv->fIoInFlight, __ATOMIC_ACQUIRE) == 0) {
        iv->fReconfiguring = false;
        return ArmUsbStreaming(iv);
    }
    iv->fRearmAfterDrain = true;   // fReconfiguring stays true — blocks zombie re-arms from leftover completions
    return kIOReturnSuccess;
}

// v28: guarantees fInReconfigure is set/cleared on every return path. Stores are
// SEQ_CST — the Dekker pair (Reconfigure: fInReconfigure store -> fStopping load /
// Stop finish: fStopping store -> fInReconfigure load) needs store-load ordering,
// which ACQ/REL cannot provide (review finding: a TOCTOU where each side passes
// before the other side publishes).
struct ReconfigureToken {
    volatile uint32_t *flag;
    explicit ReconfigureToken(volatile uint32_t *in_flag) : flag(in_flag) {
        __atomic_store_n(flag, 1, __ATOMIC_SEQ_CST);
    }
    ~ReconfigureToken() { __atomic_store_n(flag, 0, __ATOMIC_SEQ_CST); }
};

// --- APSL 2.0 derived fragment ---------------------------------------------
// The correction arithmetic below (including the m_RunningFb* identifiers) is
// ported from TEAC USB HS Audio Driver 273.4.2, TeacUSBAudioStream.cpp
// ("Modified by GFEC (TENOR)", Copyright (c) 1998-2010 Apple Computer, Inc.),
// released under the Apple Public Source License Version 2.0.
// See LICENSES/APSL-2.0.txt. Everything else in this file is original code
// (MIT, see LICENSE). Compiled for observation only (ENABLE_ASSP8802_PLL == 0);
// the active pacing path is NextOutSampleCount() below.
// ----------------------------------------------------------------------------
static inline void
ApplyAssp8802Correction(TeacAI501DA_IVars *iv, uint32_t raw)
{
    uint32_t FfWhole = (raw + 128u) & 0x00FFFF00u;
    int32_t  lDiff   = ((int32_t)FfWhole - (int32_t)iv->m_ulAvgSampleCnt) / 16;
    iv->m_RunningFbFraction += lDiff;
    iv->m_RunningFbWhole    += iv->m_RunningFbFraction / 65536;
    iv->m_RunningFbFraction  = iv->m_RunningFbFraction % 65536;
}

static inline uint32_t
NextOutSampleCount(TeacAI501DA_IVars *iv)
{
#if ENABLE_FEEDBACK_PACING
    // v27: emit whole samples by accumulating the feedback-derived Q16.16 rate.
    // fFbCurrentQ16 tracks the DAC's actual consumption (seeded to nominal until
    // the first Ff arrives), so the OUT stream follows the DAC clock — required
    // for the DAC to sustain DoP lock, and it cancels long-term drift for PCM.
    iv->fFbAccumQ16 += iv->fFbCurrentQ16;
    uint32_t samples = iv->fFbAccumQ16 >> 16;
    iv->fFbAccumQ16 &= 0xFFFF;
    return samples;
#else
    // exact rate/8000 pacing: for 44.1k yields 5,6,5,6,... with zero long-term error
    iv->fRateAccum += iv->fRateHz;
    uint32_t samples = iv->fRateAccum / MICROFRAMES_PER_SEC;
    iv->fRateAccum -= samples * MICROFRAMES_PER_SEC;
    return samples;
#endif
}

static inline IOUserAudioStreamBasicDescription
MakeFormat24(double rate)
{
    IOUserAudioStreamBasicDescription fmt = {
        .mSampleRate       = rate,
        .mFormatID         = IOUserAudioFormatID::LinearPCM,
        .mFormatFlags      = (IOUserAudioFormatFlags)
            (IOUserAudioFormatFlags::FormatFlagIsSignedInteger
           | IOUserAudioFormatFlags::FormatFlagIsPacked),
        .mBytesPerPacket   = 6,
        .mFramesPerPacket  = 1,
        .mBytesPerFrame    = 6,
        .mChannelsPerFrame = 2,
        .mBitsPerChannel   = 24,
        .mReserved         = 0,
    };
    return fmt;
}

static void
FillOutFrameList(IOUSBIsochronousFrame *frames, uint32_t numFrames, TeacAI501DA_IVars *iv)
{
    for (uint32_t i = 0; i < numFrames; i++) {
        uint32_t samples = NextOutSampleCount(iv);
        iv->fSamplesPerUF = samples;
        frames[i].status        = kIOReturnSuccess;
        frames[i].requestCount  = samples * OUT_BYTES_PER_SAMPLE;
        frames[i].completeCount = 0;
        frames[i].timeStamp     = 0;
    }
}

// v20: read back what the clock actually settled on (the 8802 silently snaps
// unsupported rates, e.g. SET_CUR 96000 on clock 12 ran at 88200)
static uint32_t
GetClockSampleRate(IOService *client, IOUSBHostDevice *device, uint8_t clockID)
{
    IOBufferMemoryDescriptor *buf = NULL;
    if (IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn, 4, 4, &buf) != kIOReturnSuccess || !buf) {
        return 0;
    }
    uint16_t xfer = 0;
    uint32_t hz = 0;
    kern_return_t ret = device->DeviceRequest(client,
        0xA1,                                   // D2H | Class | Interface
        UAC2_CUR,
        (uint16_t)(UAC2_CS_SAM_FREQ_CONTROL << 8),
        (uint16_t)((clockID << 8) | AUDIO_CONTROL_IF_NUM),
        4, buf, &xfer, 5000);
    if (ret == kIOReturnSuccess && xfer == 4) {
        IOAddressSegment seg;
        if (buf->GetAddressRange(&seg) == kIOReturnSuccess) {
            hz = ReadLE32((const volatile void *)seg.address);
        }
    }
    OSSafeReleaseNULL(buf);
    return hz;
}

// v20: dump the clock's supported rate ranges (UAC2 RANGE request)
static void
DumpClockRanges(IOService *client, IOUSBHostDevice *device, uint8_t clockID)
{
    IOBufferMemoryDescriptor *buf = NULL;
    if (IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn, 256, 4, &buf) != kIOReturnSuccess || !buf) {
        return;
    }
    uint16_t xfer = 0;
    kern_return_t ret = device->DeviceRequest(client,
        0xA1,
        0x02,                                   // RANGE
        (uint16_t)(UAC2_CS_SAM_FREQ_CONTROL << 8),
        (uint16_t)((clockID << 8) | AUDIO_CONTROL_IF_NUM),
        256, buf, &xfer, 5000);
    if (ret != kIOReturnSuccess || xfer < 2) {
        os_log(LOG, LOG_PREFIX "clock %u RANGE failed (0x%x xfer=%u)", clockID, ret, xfer);
        OSSafeReleaseNULL(buf);
        return;
    }
    IOAddressSegment seg;
    if (buf->GetAddressRange(&seg) == kIOReturnSuccess) {
        const uint8_t *p = (const uint8_t *)seg.address;
        uint16_t n = (uint16_t)(p[0] | (p[1] << 8));
        os_log(LOG, LOG_PREFIX "clock %u RANGE: %u subranges (xfer=%u)", clockID, n, xfer);
        for (uint16_t i = 0; i < n && (2 + 12*i + 12) <= xfer; i++) {
            const uint8_t *r = p + 2 + 12*i;
            uint32_t mn = (uint32_t)(r[0] | (r[1]<<8) | (r[2]<<16) | ((uint32_t)r[3]<<24));
            uint32_t mx = (uint32_t)(r[4] | (r[5]<<8) | (r[6]<<16) | ((uint32_t)r[7]<<24));
            uint32_t rs = (uint32_t)(r[8] | (r[9]<<8) | (r[10]<<16) | ((uint32_t)r[11]<<24));
            os_log(LOG, LOG_PREFIX "clock %u range[%u]: min=%u max=%u res=%u", clockID, i, mn, mx, rs);
        }
    }
    OSSafeReleaseNULL(buf);
}

// UAC2 SET_CUR CS_SAM_FREQ to ClockSource 12 via EP0 (device ownership required)
static kern_return_t
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
        UAC2_CUR,
        (uint16_t)(UAC2_CS_SAM_FREQ_CONTROL << 8),
        (uint16_t)((CLOCK_SOURCE_ID << 8) | AUDIO_CONTROL_IF_NUM),
        4, freqBuf, &xfer, 5000);
    OSSafeReleaseNULL(freqBuf);
    uint32_t cur12 = GetClockSampleRate(client, device, CLOCK_SOURCE_ID);
    uint32_t cur13 = GetClockSampleRate(client, device, 13);
    os_log(LOG, LOG_PREFIX "SET_CUR sampleRate=%u ret=0x%x xfer=%u -> clk12=%u clk13=%u",
           hz, ret, xfer, cur12, cur13);
    if (ret == kIOReturnSuccess && cur12 != 0 && cur12 != hz) {
        os_log(LOG, LOG_PREFIX "clock snapped %u -> %u", hz, cur12);
        ret = kIOReturnUnsupported;
    }
    return ret;
}

void
TeacAI501DA::free()
{
    // v28: ivars are freed at the final release (= the point where the framework can
    // no longer call methods/blocks on this object). Freeing them inside Stop's
    // deferred teardown would leave an inherent race with late callbacks that touch
    // ivars before the guards publish — the guard flags (fStopping/fInReconfigure)
    // themselves live inside ivars (review finding).
    IOSafeDeleteNULL(ivars, TeacAI501DA_IVars, 1);
    super::free();
}

// (re)start Fb polling and the pipelined OUT stream on fresh frame numbers
static kern_return_t
ArmUsbStreaming(TeacAI501DA_IVars *iv)
{
    // v28: no re-arms once Stop has begun (reachable from e.g. the rate-switch restore path)
    if (__atomic_load_n(&iv->fStopping, __ATOMIC_SEQ_CST)) {
        return kIOReturnOffline;
    }
    IOAddressSegment flSeg;
    if (iv->fFbFrameList->GetAddressRange(&flSeg) == kIOReturnSuccess) {
        IOUSBIsochronousFrame *f = (IOUSBIsochronousFrame *)flSeg.address;
        memset(f, 0, sizeof(*f));
        f->requestCount = 4;
    }
    IoInFlightInc(iv);
    kern_return_t ret = iv->fFbPipe->IsochIO(iv->fFbDataBuf, iv->fFbFrameList, 0, iv->fFbAction);
    if (ret != kIOReturnSuccess) {
        IoInFlightDec(iv);
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
    for (uint32_t s = 0; s < OUT_XFERS_IN_FLIGHT; s++) {
        IOAddressSegment ds;
        if (iv->fOutDataBufs[s]->GetAddressRange(&ds) == kIOReturnSuccess) {
            memset((void*)ds.address, 0, OUT_BUF_SIZE);
        }
        IOAddressSegment fl;
        if (iv->fOutFrameLists[s]->GetAddressRange(&fl) == kIOReturnSuccess) {
            FillOutFrameList((IOUSBIsochronousFrame *)fl.address, NUM_OUT_FRAMES, iv);
        }
        IoInFlightInc(iv);
        ret = iv->fOutPipe->IsochIO(iv->fOutDataBufs[s], iv->fOutFrameLists[s],
                                    iv->fOutFrameNumber, iv->fOutAction);
        if (ret != kIOReturnSuccess) {
            IoInFlightDec(iv);
            os_log(LOG, LOG_PREFIX "fOutPipe->IsochIO[%u]@%llu failed (0x%x)", s,
                   iv->fOutFrameNumber, ret);
            return ret;
        }
        iv->fOutFrameNumber++;
    }
    return kIOReturnSuccess;
}

kern_return_t
IMPL(TeacAI501DA, Start)
{
    kern_return_t ret;

    os_log(LOG, LOG_PREFIX "Start() entered, provider=%p", provider);

    // ---- super (IOUserAudioDriver) Start ----
    ret = Start(provider, SUPERDISPATCH);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "super Start failed (0x%x)", ret);
        return ret;
    }
    os_log(LOG, LOG_PREFIX "super Start OK");

    ivars = IONewZero(TeacAI501DA_IVars, 1);
    if (!ivars) {
        os_log(LOG, LOG_PREFIX "IONewZero(ivars) failed");
        return kIOReturnNoMemory;
    }

    // ASSP8802 slow-PLL init
    {
        uint64_t target = ((uint64_t)BASELINE_SAMPLE_RATE) << 16;
        ivars->m_ulAvgSampleCnt = (uint32_t)(target / MICROFRAMES_PER_SEC);
        ivars->m_ulAvgSampleCnt &= 0xFFFFFF00u;
    }
    ivars->fSamplesPerUF = BASELINE_SAMPLES_PER_UF;
    ivars->fNextZtsSample = ZTS_PERIOD_FRAMES;
    ivars->fRateHz = BASELINE_SAMPLE_RATE;
    ivars->fRateAccum = 0;
    ivars->fFbCurrentQ16 = NominalFbQ16(BASELINE_SAMPLE_RATE);  // v27: seed pacing until Ff arrives
    ivars->fFbAccumQ16 = 0;

    // ---- USB device-level ownership + IF#3 open + alt 1 + pipes (v4) ----
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
    os_log(LOG, LOG_PREFIX "device->Open OK");

    // v6: force a real reconfiguration. Same-value SetConfiguration is a no-op
    // (v4 lesson: completed in the same millisecond, leaving usbaudiod's
    // interface sessions alive), so drop to unconfigured first when we attach
    // to an already-configured device.
    ret = device->SetConfiguration(0, false);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "SetConfiguration(0) returned 0x%x (continuing)", ret);
    }

    // matchInterfaces=false: interfaces are created but never registered for
    // matching, so usbaudiod/HID drivers cannot claim any of them.
    ret = device->SetConfiguration(DEVICE_CONFIG_VALUE, false);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "SetConfiguration(%u) failed (0x%x)", DEVICE_CONFIG_VALUE, ret);
        goto fail_usb;
    }
    os_log(LOG, LOG_PREFIX "SetConfiguration(%u) OK", DEVICE_CONFIG_VALUE);

    {
        const IOUSBConfigurationDescriptor *cfg =
            device->CopyConfigurationDescriptorWithValue(DEVICE_CONFIG_VALUE);
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
                if (idesc && idesc->bInterfaceNumber == STREAMING_IF_NUM) {
                    iface = cand;   // CopyInterface returns retained
                    break;
                }
                OSSafeReleaseNULL(cand);
            }
            device->DestroyInterfaceIterator(iter);
        }
        if (cfg) IOUSBHostFreeDescriptor(cfg);
    }
    if (!iface) {
        os_log(LOG, LOG_PREFIX "streaming IF#%u not found", STREAMING_IF_NUM);
        ret = kIOReturnNotFound;
        goto fail_usb;
    }
    ivars->fInterface = iface;   // ownership transferred

    // v5: interface nubs may still be initializing right after SetConfiguration
    // (v4 failed with kIOReturnNotOpen in the same millisecond) — retry briefly.
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
        if (attempt == 1) {
            os_log(LOG, LOG_PREFIX "iface->Open attempt 1 failed (0x%x), retrying", ret);
        }
        IOSleep(50);
    }
    ifaceOpened = true;

    ret = iface->SelectAlternateSetting(STREAMING_ALT_SETTING);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "SelectAlternateSetting(STREAMING_ALT_SETTING) failed (0x%x)", ret);
        goto fail_usb;
    }
    os_log(LOG, LOG_PREFIX "SelectAlternateSetting(STREAMING_ALT_SETTING) OK");

    // UAC2: set ClockSource(ID 12) CS_SAM_FREQ_CONTROL via EP0 (non-fatal —
    // possible now that we own the device). v20: dump both clocks' ranges once.
    DumpClockRanges(this, device, CLOCK_SOURCE_ID);
    DumpClockRanges(this, device, 13);
    SendClockSampleRate(this, device, ivars->fRateHz);

    ret = iface->CopyPipe(0x03, &ivars->fOutPipe);
    if (ret != kIOReturnSuccess || !ivars->fOutPipe) {
        os_log(LOG, LOG_PREFIX "CopyPipe(0x03 OUT) failed (0x%x) pipe=%p", ret, ivars->fOutPipe);
        goto fail_usb;
    }
    os_log(LOG, LOG_PREFIX "CopyPipe(0x03 OUT) OK");
    ret = iface->CopyPipe(0x84, &ivars->fFbPipe);
    if (ret != kIOReturnSuccess || !ivars->fFbPipe) {
        os_log(LOG, LOG_PREFIX "CopyPipe(0x84 FB) failed (0x%x) pipe=%p", ret, ivars->fFbPipe);
        goto fail_usb;
    }
    os_log(LOG, LOG_PREFIX "CopyPipe(0x84 FB) OK");

    // Feedback buffers
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionIn, 4, 4, &ivars->fFbDataBuf);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "Create(fFbDataBuf) failed (0x%x)", ret);
        goto fail_usb;
    }
    ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                           sizeof(IOUSBIsochronousFrame), 8, &ivars->fFbFrameList);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "Create(fFbFrameList) failed (0x%x)", ret);
        goto fail_usb;
    }
    {
        IOAddressSegment seg;
        if (ivars->fFbFrameList->GetAddressRange(&seg) == kIOReturnSuccess) {
            IOUSBIsochronousFrame *f = (IOUSBIsochronousFrame *)seg.address;
            memset(f, 0, sizeof(*f));
            f->requestCount = 4;
        }
    }
    ret = CreateActionFeedbackComplete(0, &ivars->fFbAction);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "CreateActionFeedbackComplete failed (0x%x)", ret);
        goto fail_usb;
    }

    // OUT buffers (silence pre-filled), one set per in-flight transfer
    for (uint32_t s = 0; s < OUT_XFERS_IN_FLIGHT; s++) {
        ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOut, OUT_BUF_SIZE, 8,
                                               &ivars->fOutDataBufs[s]);
        if (ret != kIOReturnSuccess) {
            os_log(LOG, LOG_PREFIX "Create(fOutDataBufs[%u]) failed (0x%x)", s, ret);
            goto fail_usb;
        }
        {
            IOAddressSegment seg;
            if (ivars->fOutDataBufs[s]->GetAddressRange(&seg) == kIOReturnSuccess) {
                memset((void*)seg.address, 0, OUT_BUF_SIZE);
            }
        }
        ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                               NUM_OUT_FRAMES * sizeof(IOUSBIsochronousFrame),
                                               8, &ivars->fOutFrameLists[s]);
        if (ret != kIOReturnSuccess) {
            os_log(LOG, LOG_PREFIX "Create(fOutFrameLists[%u]) failed (0x%x)", s, ret);
            goto fail_usb;
        }
        {
            IOAddressSegment seg;
            if (ivars->fOutFrameLists[s]->GetAddressRange(&seg) == kIOReturnSuccess) {
                FillOutFrameList((IOUSBIsochronousFrame *)seg.address, NUM_OUT_FRAMES, ivars);
            }
        }
    }
    ret = CreateActionOutComplete(0, &ivars->fOutAction);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "CreateActionOutComplete failed (0x%x)", ret);
        goto fail_usb;
    }
    os_log(LOG, LOG_PREFIX "USB setup complete (pipes+buffers+actions ready)");

    // ---- AudioDriverKit objects ----
    {
        OSString *deviceUID   = OSString::withCString("jp.hogehoge.TeacAI501DA.device");
        OSString *modelUID    = OSString::withCString("AI-501DA");
        OSString *manufactUID = OSString::withCString("TEAC");

        // v17: subclass receives host-initiated rate changes
        TeacAI501DADevice *dev = OSTypeAlloc(TeacAI501DADevice);
        if (!dev || !dev->init(this, true, deviceUID, modelUID, manufactUID, ZTS_PERIOD_FRAMES)) {
            os_log(LOG, LOG_PREFIX "TeacAI501DADevice init failed");
            OSSafeReleaseNULL(dev);
            ret = kIOReturnNoResources;
            goto fail_audio;
        }
        dev->SetDriverRef(this);
        ivars->fAudioDevice = dev;   // owns the alloc reference
        ivars->fAudioDevice->SetCanBeDefaultOutputDevice(true);
        ivars->fAudioDevice->SetCanBeDefaultSystemOutputDevice(true);
        // v7: the device IS the clock device (IOUserAudioDevice subclasses
        // IOUserAudioClockDevice) — a separate clock object would never see
        // our USB zero-timestamps.
        {
            double rates[NUM_SUPPORTED_RATES];
            for (uint32_t i = 0; i < NUM_SUPPORTED_RATES; i++) {
                rates[i] = (double)kSupportedRates[i];
            }
            ivars->fAudioDevice->SetAvailableSampleRates(rates, NUM_SUPPORTED_RATES);
            ivars->fAudioDevice->SetSampleRate((double)BASELINE_SAMPLE_RATE);
            ivars->fAudioDevice->SetTransportType(IOUserAudioTransportType::USB);
            // v15/v22: without enough safety offset the HAL writes right at
            // the device-now line and the USB reader overtakes unwritten
            // regions (choppy audio at 48k, ~5% short frames at hi-res).
            // Scale it to the rate so hi-res keeps the same time head room.
            uint32_t so = SafetyOffsetForRate(ivars->fRateHz);
            uint32_t prevOff = ivars->fAudioDevice->GetOutputSafetyOffset();
            kern_return_t soRet = ivars->fAudioDevice->SetOutputSafetyOffset(so);
            os_log(LOG, LOG_PREFIX "OutputSafetyOffset %u -> %u (ret=0x%x)", prevOff, so, soRet);
        }

        // v22: ring frames == ZTS period (maudio rule). 49152 × 6B = 288KB.
        ivars->fCoreAudioBufLen = ZTS_PERIOD_FRAMES * OUT_BYTES_PER_SAMPLE;
        ret = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                               ivars->fCoreAudioBufLen, 64, &ivars->fCoreAudioBuf);
        if (ret != kIOReturnSuccess) {
            os_log(LOG, LOG_PREFIX "Create(fCoreAudioBuf) failed (0x%x)", ret);
            goto fail_audio;
        }
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
            os_log(LOG, LOG_PREFIX "IOUserAudioStream::Create failed");
            ret = kIOReturnNoResources;
            goto fail_audio;
        }
        ivars->fOutputStream = streamSp.get();
        ivars->fOutputStream->retain();
        IOUserAudioStreamBasicDescription fmts[NUM_SUPPORTED_RATES];
        for (uint32_t i = 0; i < NUM_SUPPORTED_RATES; i++) {
            fmts[i] = MakeFormat24((double)kSupportedRates[i]);
        }
        ivars->fOutputStream->SetAvailableStreamFormats(fmts, NUM_SUPPORTED_RATES);
        IOUserAudioStreamBasicDescription cur = MakeFormat24((double)BASELINE_SAMPLE_RATE);
        ivars->fOutputStream->SetCurrentStreamFormat(&cur);
        ivars->fAudioDevice->AddStream(ivars->fOutputStream);

        // v9: objects must be explicitly added to the driver, or the host
        // (coreaudiod) connects but sees no devices.
        {
            OSString *devName = OSString::withCString("TEAC AI-501DA");
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

        // ---- Phase 3b: IO operation handler ----
        // CoreAudio HAL writes interleaved 16bit/2ch PCM into fCoreAudioBuf and notifies us
        // with IOUserAudioIOOperationWriteEnd. We record the latest sample_time so OutComplete
        // can pull the right frames.
        const size_t bufLen = ivars->fCoreAudioBufLen;
        void * const  bufPtr = ivars->fCoreAudioBufPtr;
        (void)bufLen; (void)bufPtr;  // captured by reference via ivars
        ivars->fAudioDevice->SetIOOperationHandler(
            ^(IOUserAudioObjectID device,
              IOUserAudioIOOperation op,
              uint32_t frameSize,
              uint64_t sampleTime,
              uint64_t hostTime) {
                (void)device; (void)frameSize; (void)hostTime;
                // v28: this block fires in AudioDriverKit's RT context and is not
                // quiesced by cancelling the USB actions. Write nothing once teardown
                // has begun (ivars stay alive until free(), so reading fStopping is safe).
                TeacAI501DA_IVars *iv = ivars;
                if (!iv || iv->fStopping) {
                    return kIOReturnSuccess;
                }
                if (op == IOUserAudioIOOperationWriteEnd) {
                    iv->fCoreAudioWriteEndSample = sampleTime + frameSize;
                }
                return kIOReturnSuccess;
            });

        OSSafeReleaseNULL(deviceUID);
        OSSafeReleaseNULL(modelUID);
        OSSafeReleaseNULL(manufactUID);
    }

    // ---- Kick off USB IO ----
    // v11/v17: pipelined OUT + Fb, shared with rate reconfiguration
    ret = ArmUsbStreaming(ivars);
    if (ret != kIOReturnSuccess) {
        return ret;
    }

    // v7: publish our service — coreaudiod's AudioDriverKit plug-in only
    // discovers registered services (ioreg showed us as !registered).
    ret = RegisterService();
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "RegisterService failed (0x%x)", ret);
        return ret;
    }

    os_log(LOG, LOG_PREFIX "Phase 3a armed: ASSP8802 PLL + AudioDevice/Stream registered");
    return kIOReturnSuccess;

fail_audio:
    OSSafeReleaseNULL(ivars->fOutputStream);
    OSSafeReleaseNULL(ivars->fCoreAudioBuf);
    OSSafeReleaseNULL(ivars->fAudioDevice);
fail_usb:
    OSSafeReleaseNULL(ivars->fOutAction);
    for (uint32_t s = 0; s < OUT_XFERS_IN_FLIGHT; s++) {
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
IMPL(TeacAI501DA, FeedbackComplete)
{
    // v28: the token balances in-flight on every return path. Completions delivered
    // after Stop began (aborts) do the accounting only and never touch the body.
    IoInFlightToken _tok(ivars);
    if (!ivars || ivars->fStopping) {
        return;
    }
    if (status == kIOReturnSuccess) {
        IOAddressSegment seg;
        if (ivars->fFbDataBuf->GetAddressRange(&seg) == kIOReturnSuccess) {
            uint32_t raw = ReadLE32((const volatile void *)seg.address);
            ApplyAssp8802Correction(ivars, raw);   // kept for observation only (disabled PLL)
            // v27: decode explicit feedback → true rate. AI-501DA alt2 reports
            // "actual − 1", so true Q16.16 rate = raw + 1.0. Accept only values
            // within ±12.5% of nominal (reject malformed / other-rate packets so a
            // bad Ff can't corrupt pacing); otherwise keep the last good value.
            uint32_t fbQ16 = raw + (1u << 16);
            uint32_t nom = NominalFbQ16(ivars->fRateHz);
            uint32_t band = nom >> 3;
            if (fbQ16 > (nom - band) && fbQ16 < (nom + band)) {
                ivars->fFbCurrentQ16 = fbQ16;
            }
            ivars->fFbCount++;
            if ((ivars->fFbCount % 1000) == 1) {
                os_log(LOG, LOG_PREFIX "Fb #%llu raw=0x%08x fbQ16=0x%08x cur=0x%08x nom=0x%08x",
                       ivars->fFbCount, raw, fbQ16, ivars->fFbCurrentQ16, nom);
            }
        }
    } else {
        os_log(LOG, LOG_PREFIX "FeedbackComplete error 0x%x", status);
    }
    // device gone / IO torn down: stop re-arming (avoids error storm on unplug)
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
        IoInFlightInc(ivars);
        kern_return_t r = ivars->fFbPipe->IsochIO(ivars->fFbDataBuf, ivars->fFbFrameList,
                                                  0, ivars->fFbAction);
        if (r != kIOReturnSuccess) {
            IoInFlightDec(ivars);
            os_log(LOG, LOG_PREFIX "Fb re-IsochIO failed (0x%x)", r);
        }
    }
}

void
IMPL(TeacAI501DA, OutComplete)
{
    // v28: the token balances in-flight on every return path. Completions delivered
    // after Stop began (aborts) do the accounting only and never touch the body —
    // the old implementation deref'd freed ivars here and crashed (2026-07-04 20:50).
    IoInFlightToken _tok(ivars);
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
            os_log(LOG, LOG_PREFIX "OUT #%llu samples/uF=%u usbCumul=%llu caEnd=%llu bad=%llu short=%llu peak=%u lastBad=0x%08x deficitSmp=%llu worstShort=%u diverg=%llu maxDiverg=%llu overrun=%llu resyncN=%llu reanchor=%llu",
                   ivars->fOutCount, ivars->fSamplesPerUF,
                   ivars->fUsbCumulativeSamples, ivars->fCoreAudioWriteEndSample,
                   ivars->fOutBadFrames, ivars->fOutShortFrames, ivars->fPeakLevel,
                   ivars->fLastBadStatus, ivars->fShortDeficitSamples, ivars->fWorstShortSamples,
                   (ivars->fUsbCumulativeSamples >= ivars->fUsbCompletedSamples
                    ? ivars->fUsbCumulativeSamples - ivars->fUsbCompletedSamples : 0),
                   ivars->fMaxDivergence, ivars->fRingOverrunEvents, ivars->fResyncCount,
                   ivars->fRingGuardReanchors);
            ivars->fPeakLevel = 0;
            ivars->fWorstShortSamples = 0;   // v23: per-interval max, reset each summary
        }
    }

    if (!ivars->fReconfiguring && ivars->fOutPipe && ivars->fOutAction) {
        // v11: completions arrive in scheduling order — round-robin slot
        uint32_t slot = ivars->fOutSlot;
        ivars->fOutSlot = (slot + 1) % OUT_XFERS_IN_FLIGHT;
        IOBufferMemoryDescriptor *dataBuf   = ivars->fOutDataBufs[slot];
        IOBufferMemoryDescriptor *frameList = ivars->fOutFrameLists[slot];

        // 0) v12: inspect completed frame statuses before refilling
        //    v16: also count completed samples — the ZTS clock must reflect
        //    what actually hit the wire, not what we queued (4 transfers early).
        IOAddressSegment flSeg;
        uint32_t totalSamplesNext = 0;
        if (frameList->GetAddressRange(&flSeg) == kIOReturnSuccess) {
            IOUSBIsochronousFrame *frames = (IOUSBIsochronousFrame *)flSeg.address;
            uint32_t completedBytes = 0;
            for (uint32_t i = 0; i < NUM_OUT_FRAMES; i++) {
                IOReturn st = frames[i].status;
                if (st != kIOReturnSuccess) {
                    ivars->fOutBadFrames++;
                    ivars->fLastBadStatus = (uint32_t)st;   // v23: capture the code
                    // v23: log the very first failure with full context (frame
                    // index, requested vs completed, rate). One-shot so a per-
                    // frame failure at hi-res can't spam the kernel log.
                    if (!ivars->fFirstBadLogged) {
                        os_log(LOG, LOG_PREFIX "first bad frame status 0x%08x at #%llu frame %u req=%u done=%u rate=%u",
                               (uint32_t)st, ivars->fOutCount, i,
                               frames[i].requestCount, frames[i].completeCount, ivars->fRateHz);
                        ivars->fFirstBadLogged = true;
                    }
                } else if (frames[i].completeCount < frames[i].requestCount) {
                    ivars->fOutShortFrames++;
                    // v23: how many samples this short frame actually dropped
                    uint32_t deficit = (frames[i].requestCount - frames[i].completeCount) / OUT_BYTES_PER_SAMPLE;
                    ivars->fShortDeficitSamples += deficit;
                    if (deficit > ivars->fWorstShortSamples) ivars->fWorstShortSamples = deficit;
                }
                completedBytes += frames[i].completeCount;
            }
            ivars->fUsbCompletedSamples += completedBytes / OUT_BYTES_PER_SAMPLE;
        }

        // 1) Compute next frame sizes via ASSP8802 slow-PLL
        if (flSeg.address) {
            IOUSBIsochronousFrame *frames = (IOUSBIsochronousFrame *)flSeg.address;
            FillOutFrameList(frames, NUM_OUT_FRAMES, ivars);
            for (uint32_t i = 0; i < NUM_OUT_FRAMES; i++) {
                totalSamplesNext += frames[i].requestCount / OUT_BYTES_PER_SAMPLE;
            }
        }

        // 2) Phase 3b: copy CoreAudio ring -> USB OUT buffer
        // v15: back to contiguous packing — the kernel shim assigns
        // txn[i].offset = running sum of requestCount (verified by
        // disassembling IOUSBHostPipe::io in BootKernelExtensions.kc), and
        // the v14 stride experiment audibly made things worse.
        if (ivars->fCoreAudioBufPtr && dataBuf) {
            IOAddressSegment outSeg;
            if (dataBuf->GetAddressRange(&outSeg) == kIOReturnSuccess) {
                uint8_t *out = (uint8_t *)outSeg.address;
                const uint8_t *src = (const uint8_t *)ivars->fCoreAudioBufPtr;
                size_t srcLen = ivars->fCoreAudioBufLen;

                // v25 (cause A guard): if the read window overtakes CoreAudio's write
                // position (caEnd), we read the unwritten stale ring — loud bursts /
                // DoP breakdown. During playback (write advancing), when about to
                // overtake, re-anchor the reader to safety_offset below write so the
                // most recent real audio is emitted. The healthy path is unchanged
                // (read normally sits below write's safety margin and never triggers);
                // this fires only on overtake. Structurally forbids stale reads
                // (= audible breakdown).
                {
                    uint64_t writePos = ivars->fCoreAudioWriteEndSample;
                    bool playing = (writePos != ivars->fPrevWriteEnd);
                    if (playing &&
                        (ivars->fUsbCumulativeSamples + totalSamplesNext) > writePos) {
                        uint32_t safety = SafetyOffsetForRate(ivars->fRateHz);
                        uint64_t safeStart = (writePos > (uint64_t)safety)
                                           ? (writePos - safety) : 0;
                        if (safeStart < ivars->fUsbCumulativeSamples) {
                            ivars->fUsbCumulativeSamples = safeStart;  // pull the reader back below write
                            ivars->fRingGuardReanchors++;
                        }
                    }
                }
                uint64_t startSample = ivars->fUsbCumulativeSamples;

                size_t bytesNeeded = totalSamplesNext * OUT_BYTES_PER_SAMPLE;
                size_t outPos = 0;
                for (uint32_t i = 0; i < NUM_OUT_FRAMES && outPos < bytesNeeded; i++) {
                    IOUSBIsochronousFrame *frames = (IOUSBIsochronousFrame *)flSeg.address;
                    uint32_t fb = frames[i].requestCount;
                    if (outPos + fb > bytesNeeded) break;
                    for (uint32_t b = 0; b < fb; b++) {
                        size_t srcIdx = ((startSample * OUT_BYTES_PER_SAMPLE) + outPos + b) % srcLen;
                        out[outPos + b] = src[srcIdx];
                    }
                    outPos += fb;
                }
                // peak of the chunk just copied (0 = silence): approximate
                // from the top 2 bytes of each 3-byte 24bit sample
                for (size_t b = 0; b + 2 < outPos; b += 3) {
                    int16_t v = (int16_t)((uint16_t)out[b+1] | ((uint16_t)out[b+2] << 8));
                    uint32_t a = (v < 0) ? (uint32_t)(-(int32_t)v) : (uint32_t)v;
                    if (a > ivars->fPeakLevel) ivars->fPeakLevel = a;
                }
                // v26 (observation only): DoP marker inspection. In DoP the top byte of
                // each 24-bit sample alternates 0x05/0xFA (same on L/R, flipping every
                // frame). Log the MSBs of out[] just before transmission once per second
                // to directly confirm the dext is delivering valid DoP markers (behavior
                // unchanged). With PCM material the top bytes are real audio, i.e. noise-like.
                if ((ivars->fOutCount % 1000) == 1 && outPos >= 48) {
                    os_log(LOG, LOG_PREFIX "MSB16= %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                           out[2],out[5],out[8],out[11],out[14],out[17],out[20],out[23],
                           out[26],out[29],out[32],out[35],out[38],out[41],out[44],out[47]);
                }
                ivars->fUsbCumulativeSamples += totalSamplesNext;

                // v24 (observation only): evidence for cause A. How far read (scheduled)
                // has drifted from completed (the ZTS axis) = deficit sum, and whether
                // read overtakes write (caEnd) during playback and reads the stale ring.
                // Behavior unchanged.
                uint64_t diverg = (ivars->fUsbCumulativeSamples >= ivars->fUsbCompletedSamples)
                                ? (ivars->fUsbCumulativeSamples - ivars->fUsbCompletedSamples) : 0;
                if (diverg > ivars->fMaxDivergence) ivars->fMaxDivergence = diverg;
                bool writing = (ivars->fCoreAudioWriteEndSample != ivars->fPrevWriteEnd);
                ivars->fPrevWriteEnd = ivars->fCoreAudioWriteEndSample;
                if (writing &&
                    ivars->fCoreAudioWriteEndSample < ivars->fUsbCumulativeSamples) {
                    ivars->fRingOverrunEvents++;   // read overtook write (= stale read)
                }
            }
        }

        // 3) Phase 3c/v10/v16: zero-timestamp contract — update exactly once
        // per ZTS period when the COMPLETED sample count crosses a boundary,
        // sample_time floor-aligned to the boundary, host time = raw
        // mach_absolute_time() (maudio tried interpolated/synthetic host
        // times and reverted to raw — the HAL filters jitter itself).
        if (ivars->fAudioDevice &&
            ivars->fUsbCompletedSamples >= ivars->fNextZtsSample) {
            ivars->fAudioDevice->UpdateCurrentZeroTimestamp(ivars->fNextZtsSample,
                                                            mach_absolute_time());
            ivars->fNextZtsSample += ZTS_PERIOD_FRAMES;
        }

        // 4) Re-enqueue this slot at the next consecutive frame number
        IoInFlightInc(ivars);
        kern_return_t r = ivars->fOutPipe->IsochIO(dataBuf, frameList,
                                                   ivars->fOutFrameNumber, ivars->fOutAction);
        if (r == kIOReturnSuccess) {
            ivars->fOutFrameNumber++;
        } else {
            IoInFlightDec(ivars);
            // fell behind (e.g. frame in the past) — resync to current frame
            uint64_t f = 0;
            if (ivars->fDevice &&
                ivars->fDevice->GetFrameNumber(&f, NULL) == kIOReturnSuccess) {
                ivars->fResyncCount++;   // v24 (observation): cause B — firstFrameNumber fell into the past
                os_log(LOG, LOG_PREFIX "OUT re-IsochIO 0x%x @%llu — resync to %llu (resyncN=%llu)",
                       r, ivars->fOutFrameNumber, f + 4, ivars->fResyncCount);
                ivars->fOutFrameNumber = f + 4;
                IoInFlightInc(ivars);
                r = ivars->fOutPipe->IsochIO(dataBuf, frameList,
                                             ivars->fOutFrameNumber, ivars->fOutAction);
                if (r == kIOReturnSuccess) {
                    ivars->fOutFrameNumber++;
                } else {
                    IoInFlightDec(ivars);
                    os_log(LOG, LOG_PREFIX "OUT resync IsochIO failed (0x%x)", r);
                }
            }
        }
    }
}

kern_return_t
TeacAI501DA::ReconfigureForRate(uint32_t in_rate_hz)
{
    if (!ivars || !ivars->fInterface || !ivars->fDevice ||
        !ivars->fOutPipe || !ivars->fFbPipe) {
        return kIOReturnNotReady;
    }
    bool supported = false;
    for (uint32_t i = 0; i < NUM_SUPPORTED_RATES; i++) {
        if (kSupportedRates[i] == in_rate_hz) { supported = true; break; }
    }
    if (!supported) {
        os_log(LOG, LOG_PREFIX "ReconfigureForRate: unsupported %u", in_rate_hz);
        return kIOReturnUnsupported;
    }
    if (in_rate_hz == ivars->fRateHz && !ivars->fReconfiguring) {
        // v28: fReconfiguring still set = the previous switch was interrupted (e.g.
        // quiesce timeout) and the stream is stopped. Do a full reconfigure to recover
        // even when the requested rate is unchanged.
        return kIOReturnSuccess;
    }

    // v28: publish "reconfigure in progress" — a concurrent Stop's deferred teardown
    // waits for this to reach 0, so it won't demolish the ground this method stands
    // on (ivars/pipes) first (window D). RAII: cleared on every return path.
    // The ordering is the essence (Dekker): publish fInReconfigure first, then check
    // fStopping. In the reverse order there is a gap ("checked but not yet published")
    // where Stop's finish sees 0, completes the release, and this method resumes onto
    // freed ivars/pipes (the TOCTOU from review).
    ReconfigureToken _reconf(&ivars->fInReconfigure);
    if (__atomic_load_n(&ivars->fStopping, __ATOMIC_SEQ_CST)) {
        return kIOReturnOffline;
    }
    os_log(LOG, LOG_PREFIX "ReconfigureForRate %u -> %u", ivars->fRateHz, in_rate_hz);

    // stop USB IO. Abort completions line up behind us on this same default queue,
    // so never sleep-wait here (v28's 5s wait always timed out — measured). Same
    // queue also means no handler runs while this method executes, so releasing and
    // re-acquiring pipes cannot race. Zombie re-arms from leftover completions are
    // blocked by fReconfiguring (stays true until the drain completes); the re-arm
    // is scheduled after the drain via ScheduleRearmAfterDrain.
    ivars->fReconfiguring = true;
    ivars->fOutPipe->Abort(0, kIOReturnAborted, NULL);
    ivars->fFbPipe->Abort(0, kIOReturnAborted, NULL);

    ivars->fInterface->SelectAlternateSetting(0);
    kern_return_t ret = SendClockSampleRate(this, ivars->fDevice, in_rate_hz);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "clock SET_CUR %u failed (0x%x) — restoring %u",
               in_rate_hz, ret, ivars->fRateHz);
        SendClockSampleRate(this, ivars->fDevice, ivars->fRateHz);
        ivars->fInterface->SelectAlternateSetting(STREAMING_ALT_SETTING);
        ScheduleRearmAfterDrain(ivars);   // v29: re-arm at the old rate after the drain
        return ret;
    }
    ret = ivars->fInterface->SelectAlternateSetting(STREAMING_ALT_SETTING);
    if (ret != kIOReturnSuccess) {
        os_log(LOG, LOG_PREFIX "SelectAlternateSetting(STREAMING_ALT_SETTING) failed (0x%x)", ret);
        // v29: fReconfiguring stays true — blocks re-arms from leftover completions.
        // The stream stops, but the next switch (same rate allowed) recovers it via a full reconfigure
        return ret;
    }

    // v19: alt-setting cycling recreates the endpoints — the old pipe objects
    // go NotReady. Re-copy them before re-arming.
    OSSafeReleaseNULL(ivars->fOutPipe);
    OSSafeReleaseNULL(ivars->fFbPipe);
    ret = ivars->fInterface->CopyPipe(0x03, &ivars->fOutPipe);
    if (ret != kIOReturnSuccess || !ivars->fOutPipe) {
        os_log(LOG, LOG_PREFIX "re-CopyPipe(0x03) failed (0x%x)", ret);
        return ret ? ret : kIOReturnNotFound;   // v29: fReconfiguring left true
    }
    ret = ivars->fInterface->CopyPipe(0x84, &ivars->fFbPipe);
    if (ret != kIOReturnSuccess || !ivars->fFbPipe) {
        os_log(LOG, LOG_PREFIX "re-CopyPipe(0x84) failed (0x%x)", ret);
        return ret ? ret : kIOReturnNotFound;   // v29: fReconfiguring left true
    }

    // reset pacing + the shared sample-time axis, re-anchor the HAL clock
    // (host IO is stopped during PerformDeviceConfigurationChange)
    ivars->fRateHz = in_rate_hz;
    ivars->fRateAccum = 0;
    ivars->fFbCurrentQ16 = NominalFbQ16(in_rate_hz);  // v27: re-seed pacing for new rate
    ivars->fFbAccumQ16 = 0;
    ivars->fUsbCumulativeSamples = 0;
    ivars->fUsbCompletedSamples = 0;
    ivars->fCoreAudioWriteEndSample = 0;
    ivars->fPrevWriteEnd = 0;
    // v24 (#26): reset per-rate observations (no cross-switch mixing; one-shots re-fire).
    // Cumulative behavior-neutral observations (fMaxDivergence/fRingOverrunEvents/fResyncCount) are kept.
    ivars->fOutBadFrames = 0;
    ivars->fOutShortFrames = 0;
    ivars->fShortDeficitSamples = 0;
    ivars->fWorstShortSamples = 0;
    ivars->fLastBadStatus = 0;
    ivars->fFirstBadLogged = false;
    ivars->fPeakLevel = 0;
    ivars->fNextZtsSample = ZTS_PERIOD_FRAMES;
    if (ivars->fCoreAudioBufPtr) {
        memset(ivars->fCoreAudioBufPtr, 0, ivars->fCoreAudioBufLen);
    }
    if (ivars->fOutputStream) {
        IOUserAudioStreamBasicDescription fmt = MakeFormat24((double)in_rate_hz);
        ivars->fOutputStream->SetCurrentStreamFormat(&fmt);
    }
    if (ivars->fAudioDevice) {
        // v22: re-scale safety offset for the new rate (config change is in
        // progress, so the host tolerates the change), then re-anchor the clock
        uint32_t so = SafetyOffsetForRate(in_rate_hz);
        ivars->fAudioDevice->SetOutputSafetyOffset(so);
        os_log(LOG, LOG_PREFIX "OutputSafetyOffset -> %u for %u", so, in_rate_hz);
        ivars->fAudioDevice->UpdateCurrentZeroTimestamp(0, mach_absolute_time());
    }

    // v29: re-arm after leftover completions drain (same queue, so sleep-waiting is impossible)
    ret = ScheduleRearmAfterDrain(ivars);
    os_log(LOG, LOG_PREFIX "rate %u armed (ret=0x%x deferred=%d)", in_rate_hz, ret,
           ivars->fRearmAfterDrain ? 1 : 0);
    return ret;
}

kern_return_t
IMPL(TeacAI501DA, Stop)
{
    // v28: the old implementation released everything and completed
    // IOSafeDeleteNULL(ivars) before the abort completions queued behind it on the
    // default queue were delivered -> the next handler deref'd NULL ivars -> SIGSEGV
    // (real crash 2026-07-04 20:50, during a disable operation). Now follows the
    // IOService.iig canon of "hand off to super after quiesce completes":
    // fStopping -> Close (implicitly aborts pipe IO) -> OSAction::Cancel (its handler
    // fires "after all callbacks have finished", per OSAction.iig) -> release inside
    // the Cancel handler + deferred Stop(provider, SUPERDISPATCH).
    if (!ivars) {
        return Stop(provider, SUPERDISPATCH);
    }
    os_log(LOG, LOG_PREFIX "Stop: fb=%llu out=%llu inflight=%u — quiescing",
           ivars->fFbCount, ivars->fOutCount,
           __atomic_load_n(&ivars->fIoInFlight, __ATOMIC_ACQUIRE));
    // SEQ_CST — forms the Dekker pair with ReconfigureForRate's "publish
    // fInReconfigure -> check fStopping" (finish's fInReconfigure load is SEQ_CST too)
    __atomic_store_n(&ivars->fStopping, true, __ATOMIC_SEQ_CST);

    if (ivars->fInterface) {
        ivars->fInterface->SelectAlternateSetting(0);   // implicitly aborts all pipe IO
        ivars->fInterface->Close(this, 0);
    }
    if (ivars->fDevice) {
        ivars->fDevice->Close(this, 0);
    }

    // The release and super::Stop are deferred until "all callbacks have finished".
    // The block captures this and provider (DriverKit keeps provider retained until
    // super::Stop is called). The Cancel handler is _Block_copy'd inside the Cancel
    // implementation, so passing a stack block is safe (verified against the runtime
    // binary during review).
    void (^finish)(void) = ^{
        os_log(LOG, LOG_PREFIX "Stop: cancel handler fired");   // evidence log for Cancel's asynchrony
        // Also wait for a concurrent rate switch (AudioDriverKit work queue) to finish
        // using ivars/pipes (window D). No upper bound — ReconfigureToken is RAII, so
        // the wait being finite is structurally guaranteed (<= ~40s under USB timeouts,
        // normally ms-scale. The old 30s cap was shorter than a wedged device's EP0
        // chain, and forcing the release on expiry would reintroduce the very UAF this
        // fixes — review finding).
        uint32_t waitedMs = 0;
        while (__atomic_load_n(&ivars->fInReconfigure, __ATOMIC_SEQ_CST) != 0) {
            IOSleep(5);
            waitedMs += 5;
            if ((waitedMs % 5000) == 0) {
                os_log(LOG, LOG_PREFIX "Stop: still waiting for reconfigure (%ums)", waitedMs);
            }
        }
        // Note: fIoInFlight may never return to 0 — +1s from completions whose
        // delivery Cancel dropped can remain. Never wait for 0 on this counter
        // after issuing Cancel.
        os_log(LOG, LOG_PREFIX "Stop: teardown (reconf-wait %ums, inflight=%u incl. cancel-orphaned)",
               waitedMs, __atomic_load_n(&ivars->fIoInFlight, __ATOMIC_ACQUIRE));
        OSSafeReleaseNULL(ivars->fOutputStream);
        OSSafeReleaseNULL(ivars->fCoreAudioBuf);
        OSSafeReleaseNULL(ivars->fAudioDevice);
        OSSafeReleaseNULL(ivars->fOutAction);
        for (uint32_t s = 0; s < OUT_XFERS_IN_FLIGHT; s++) {
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
        // ivars themselves are not freed here — they stay alive until free() (final
        // release) so that late callbacks can still read fStopping safely (review finding)
        Stop(provider, SUPERDISPATCH);
    };

    OSAction *outAction = ivars->fOutAction;
    OSAction *fbAction  = ivars->fFbAction;
    kern_return_t cr;
    if (outAction && fbAction) {
        cr = outAction->Cancel(^{
            kern_return_t cr2 = fbAction->Cancel(finish);
            if (cr2 != kIOReturnSuccess) {
                os_log(LOG, LOG_PREFIX "fbAction->Cancel failed (0x%x) — sync teardown", cr2);
                finish();
            }
        });
        if (cr != kIOReturnSuccess) {
            os_log(LOG, LOG_PREFIX "outAction->Cancel failed (0x%x) — sync teardown", cr);
            finish();
        } else {
            os_log(LOG, LOG_PREFIX "Stop: Cancel returned (async)");   // evidence log
        }
        return kIOReturnSuccess;
    }
    if (outAction || fbAction) {
        OSAction *act = outAction ? outAction : fbAction;
        cr = act->Cancel(finish);
        if (cr != kIOReturnSuccess) {
            os_log(LOG, LOG_PREFIX "Cancel failed (0x%x) — sync teardown", cr);
            finish();
        }
        return kIOReturnSuccess;
    }
    finish();   // actions never created (early Start failure) — nothing to wait for, tear down immediately
    return kIOReturnSuccess;
}
