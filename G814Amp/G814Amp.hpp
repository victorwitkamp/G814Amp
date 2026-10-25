// G814Amp: CS35L41 x2 (SPI, behind ALC294) speaker amplifier driver for the Asus ROG Strix G814JI.
// Control path: PCH GSPI3 (PCI 8086:7A79, Intel LPSS SSP, PIO) + GPIO pads GPP_S0 (reset), GPP_D20 (CS of amp 1).
// Audio data reaches the amps over the codec's I2S (AppleHDA / AppleALC); this driver only powers and configures
// them. The CS35L41 sequences are ported from Linux (sound/soc/codecs/cs35l41-lib.c and
// sound/hda/codecs/side-codecs/cs35l41_hda.c, GPL-2.0); the register tables are in cs35l41_regs.h / cs35l41_otp_maps.h.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOMemoryDescriptor.h>

#define G814AMP_NAMPS 2

class G814Amp : public IOService {
    OSDeclareDefaultStructors(G814Amp)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn setPowerState(unsigned long state, IOService *dev) override;
    void systemWillShutdown(IOOptionBits specifier) override;

    // Commands (sysctl debug.g814amp.cmd):
    //  0 = read-only survey (PCI, LPSS caps, SSP regs, pad config/lock/ownership, no writes)
    //  1 = probe: SSP bring-up, reset pulse, SFT_RESET, OTP boot, DEVID/REVID/OTPID of both amps
    //  2 = configure both amps (errata, OTP trims, boost, GPIO, ASP, muted, GLOBAL_EN off)
    //  3 = enable: AMP_EN + GLOBAL_EN, PUP_DONE, unmute at debug.g814amp.gain; starts the fault poll
    //  4 = status dump (PLL lock, IRQ status, power regs) of both amps
    //  5 = disable: mute, GLOBAL_EN off (PDN_DONE), AMP_EN off, error release
    //  6 = disable + reset low (amps in reset)
    //  9 = 1 + 2 + 3
    int runCommand(int cmd);

private:
    // hardware
    IOPCIDevice *pci = nullptr;
    IOMemoryMap *barMap = nullptr;
    volatile uint8_t *bar = nullptr;
    uint64_t barPhys = 0;
    IOMemoryDescriptor *gpioMD[3] = {};
    IOMemoryMap *gpioMap[3] = {};
    volatile uint8_t *gpio[3] = {};      // 0 = community with GPP_S, 1 = GPP_D, 2 = GPP_G

    IOLock *lock = nullptr;
    IOWorkLoop *wl = nullptr;
    IOTimerEventSource *timer = nullptr;

    bool sspReady = false;
    bool probed[G814AMP_NAMPS] = {};
    bool configured[G814AMP_NAMPS] = {};
    bool enabled[G814AMP_NAMPS] = {};
    bool wantEnabled = false;             // enable requested; re-applied after wake
    uint32_t devid[G814AMP_NAMPS] = {}, revid[G814AMP_NAMPS] = {}, otpid[G814AMP_NAMPS] = {};
    uint32_t faultCount = 0;

    // log ring published as the "log" property (ioreg) and via IOLog
    char logbuf[12288] = {};
    size_t loglen = 0;
    void logf(const char *fmt, ...) __printflike(2, 3);
    void publishLog();
    void setStatus(const char *fmt, ...) __printflike(2, 3);

    // MMIO helpers
    inline uint32_t r32(volatile uint8_t *b, uint32_t off) { return *(volatile uint32_t *)(b + off); }
    inline void w32(volatile uint8_t *b, uint32_t off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }

    // PCI / LPSS / SSP
    bool pciWake();
    bool mapAll();
    void unmapAll();
    bool sspInit();
    bool sspXfer(int amp, const uint8_t *tx, uint8_t *rx, size_t n);
    void csSet(int amp, bool assert);

    // GPIO pads
    bool padSetTx(int comm, uint32_t padcfg0Off, bool high);
    bool padGetTx(int comm, uint32_t padcfg0Off);
    void padSurvey();

    // CS35L41 register access
    bool ampWrite(int amp, uint32_t reg, uint32_t val);
    bool ampRead(int amp, uint32_t reg, uint32_t *val);
    bool ampUpdate(int amp, uint32_t reg, uint32_t mask, uint32_t val);
    bool ampPoll(int amp, uint32_t reg, uint32_t mask, uint32_t want, int tries, int ms);

    // CS35L41 sequences
    bool ampProbeAll();
    bool ampProbe(int amp);
    bool ampConfigure(int amp);
    bool ampErrata(int amp);
    bool ampOtpUnpack(int amp);
    bool ampBoostConfig(int amp);
    bool ampEnable(int amp);
    bool ampUnmute(int amp);
    bool ampMute(int amp);
    bool ampDisable(int amp);
    void ampStatus(int amp);
    uint32_t ampErrors(int amp, uint32_t *status1);
    void ampIrqRelease(int amp);
    void allOff(bool resetLow);

    // fault poll (1 s) + auto mode tick
    static void timerFired(OSObject *owner, IOTimerEventSource *src);
    void pollFaults();

    // auto mode (Info.plist AutoEnable, default on; boot-arg -g814ampmanual = off; sysctl debug.g814amp.auto 0/1):
    // follow AppleHDA's output engine: IOAudioEngineState 1 (running, BCLK present) => probe + configure + enable;
    // 0 => mute + GLOBAL_EN off. Bounded retries.
    bool autoMode = true;
    IONotifier *hdaNotifier = nullptr;
    IOService *hdaEngine = nullptr;       // retained AppleHDAEngineOutput
    int autoRetries = 0;
    bool autoGaveUp = false;
    static bool hdaPublished(void *target, void *ref, IOService *svc, IONotifier *n);
    bool engineRunning();
    void autoTick();                      // called under lock
};
