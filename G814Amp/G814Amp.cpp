// G814Amp.cpp: see G814Amp.hpp.
#include "G814Amp.hpp"
#include "cs35l41_regs.h"
#include "cs35l41_otp_maps.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOMessage.h>
#include <pexpert/pexpert.h>
#include <libkern/OSByteOrder.h>
#include <sys/sysctl.h>
#include <stdarg.h>

#define super IOService
OSDefineMetaClassAndStructors(G814Amp, IOService)

// ---------------------------------------------------------------------------------------------------------------
// Platform constants
// ---------------------------------------------------------------------------------------------------------------
// GPIO communities (P2SB sideband 0xE0000000 + port << 16). Pad registers live in the first 4 KiB page.
static const uint64_t kGpioCommPhys[3] = { 0xE06A0000ULL /* GPP_S.. */, 0xE0690000ULL /* GPP_D.. */, 0xE06D0000ULL /* GPP_B, GPP_G.. */ };
enum { COMM_S = 0, COMM_D = 1, COMM_G = 2 };
#define PAD_RESET   0x700   // GPP_S0  PADCFG0: amps' reset (both), active low
#define PAD_IRQ     0x710   // GPP_S1  PADCFG0: amps' INTb (input, not used yet)
#define PAD_CS1     0x840   // GPP_D20 PADCFG0: chip select of amp #1 (GPIO, active low)
#define PAD_SPKID   0x880   // GPP_G0  PADCFG0: speaker id (input)
#define PADCFG1_OFF 0x4
#define PAD_OWN     0x0A0
#define PADCFGLOCK  0x110   // + group*8; PADCFGLOCKTX at +4
#define HOSTSW_OWN  0x150   // + group*4
// PADCFG0 bits (Linux pinctrl-intel.h)
#define PADCFG0_GPIOTXSTATE  (1u << 0)
#define PADCFG0_GPIORXSTATE  (1u << 1)
#define PADCFG0_GPIOTXDIS    (1u << 8)
#define PADCFG0_GPIORXDIS    (1u << 9)
#define PADCFG0_PMODE_MASK   (3u << 10)

// Intel LPSS SSP (spi-pxa2xx LPSS_CNL_SSP + intel-lpss)
#define SSCR0 0x00
#define SSCR1 0x04
#define SSSR  0x08
#define SSDR  0x10
#define SSTO  0x28
#define SSITF 0x44
#define SSIRF 0x48
#define SSCR0_DSS8      0x7           // 8-bit data
#define SSCR0_SSE       (1u << 7)
#define SSSR_TNF        (1u << 2)
#define SSSR_RNE        (1u << 3)
#define SSSR_BSY        (1u << 4)
#define SSSR_ROR        (1u << 7)
#define SSSR_TFL_MASK   (0xFu << 8)
#define SSSR_RFL_MASK   (0xFu << 12)
#define PRIV            0x200
#define PRIV_CLOCK      (PRIV + 0x00)   // bit0 enable, M bits 15:1, N bits 30:16, bit31 update
#define PRIV_RESETS     (PRIV + 0x04)   // 0 = in reset; 0x7 = FUNC | IDMA released
#define PRIV_ACTIVELTR  (PRIV + 0x10)
#define PRIV_IDLELTR    (PRIV + 0x14)
#define PRIV_SSP_REG    (PRIV + 0x20)
#define PRIV_CS_CTRL    (PRIV + 0x24)   // bit0 SW_MODE, bit1 CS_HIGH (deasserted), bits 9:8 CS select
#define PRIV_CLOCK_GATE (PRIV + 0x38)   // bits 1:0: 3 = force on, 0 = force off (dynamic gating)
#define PRIV_REMAP_ADDR (PRIV + 0x40)
#define PRIV_CAPS       (PRIV + 0xFC)
#define CS_CTRL_SW_MODE (1u << 0)
#define CS_CTRL_CS_HIGH (1u << 1)
#define LPSS_CLK_HZ     100000000u
#define SPI_HZ          4000000u

// CS35L41 registers Linux names by raw address in the errata tables
#define CS35L41_UNDOC_4310 0x00004310

// ---------------------------------------------------------------------------------------------------------------
// sysctl: debug.g814amp.{cmd,gain,state,status}
// ---------------------------------------------------------------------------------------------------------------
static G814Amp *gInstance = nullptr;
static int gGain = 4;        // AMP_GAIN_PCM code: 4 = 4.5 dB (Linux DSP-bypass level). 9 = 9.5 dB (BIOS POST sound).
static int gState = 0;       // bit0 ssp, bit1 probed, bit2 configured, bit3 enabled, bit8 fault
static char gStatus[256] = "not started";
static int gAuto = 1;        // auto mode live switch (0 after cmd 5/6 by hand; 1 resumes)

static int g814amp_cmd_sysctl SYSCTL_HANDLER_ARGS {
    int v = 0;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (!gInstance) return ENXIO;
    return gInstance->runCommand(v);
}

SYSCTL_NODE(_debug, OID_AUTO, g814amp, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "G814JI CS35L41 speaker amps");
SYSCTL_PROC(_debug_g814amp, OID_AUTO, cmd, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY, 0, 0,
            g814amp_cmd_sysctl, "I", "run a step: 0 survey, 1 probe, 2 configure, 3 enable, 4 status, 5 disable, 6 reset, 9 = 1+2+3");
SYSCTL_INT(_debug_g814amp, OID_AUTO, gain, CTLFLAG_RW | CTLFLAG_LOCKED, &gGain, 0, "AMP_GAIN_PCM code 0..9 (4 = 4.5 dB)");
SYSCTL_INT(_debug_g814amp, OID_AUTO, state, CTLFLAG_RD | CTLFLAG_LOCKED, &gState, 0, "bit0 ssp bit1 probed bit2 configured bit3 enabled bit8 fault");
SYSCTL_STRING(_debug_g814amp, OID_AUTO, status, CTLFLAG_RD | CTLFLAG_LOCKED, gStatus, 0, "last verdict line");
SYSCTL_INT(_debug_g814amp, OID_AUTO, auto, CTLFLAG_RW | CTLFLAG_LOCKED, &gAuto, 0, "1 = amps follow AppleHDA's output engine (0 after cmd 5/6)");

static struct sysctl_oid *gOids[] = { &sysctl__debug_g814amp, &sysctl__debug_g814amp_cmd, &sysctl__debug_g814amp_gain,
                                      &sysctl__debug_g814amp_state, &sysctl__debug_g814amp_status, &sysctl__debug_g814amp_auto };

// ---------------------------------------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------------------------------------
void G814Amp::logf(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line)) n = sizeof(line) - 1;
    IOLog("G814Amp: %s\n", line);
    if (loglen + (size_t)n + 2 >= sizeof(logbuf)) {        // drop the oldest half
        size_t keep = sizeof(logbuf) / 2;
        memmove(logbuf, logbuf + (loglen - keep), keep);
        loglen = keep;
    }
    memcpy(logbuf + loglen, line, (size_t)n);
    loglen += (size_t)n;
    logbuf[loglen++] = '\n';
    logbuf[loglen] = 0;
}

void G814Amp::publishLog() {
    setProperty("log", logbuf);
    setProperty("status", gStatus);
    setProperty("state", (unsigned long long)gState, 32);
}

void G814Amp::setStatus(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gStatus, sizeof(gStatus), fmt, ap);
    va_end(ap);
    logf("STATUS %s", gStatus);
}

// ---------------------------------------------------------------------------------------------------------------
// IOService
// ---------------------------------------------------------------------------------------------------------------
static IOPMPowerState gPMStates[2] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMPowerOn | kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};

bool G814Amp::start(IOService *provider) {
    if (!super::start(provider)) return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) { IOLog("G814Amp: provider is not an IOPCIDevice\n"); return false; }
    uint32_t id = pci->configRead32(kIOPCIConfigVendorID);
    if (id != 0x7A798086u) { IOLog("G814Amp: unexpected PCI id %08x\n", id); return false; }

    lock = IOLockAlloc();
    if (!lock) return false;
    wl = getWorkLoop();
    if (wl) {
        timer = IOTimerEventSource::timerEventSource(this, timerFired);
        if (timer && wl->addEventSource(timer) != kIOReturnSuccess) { timer->release(); timer = nullptr; }
    }
    gInstance = this;
    for (size_t i = 0; i < sizeof(gOids) / sizeof(gOids[0]); i++) sysctl_register_oid(gOids[i]);

    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, gPMStates, 2);

    OSBoolean *ab = OSDynamicCast(OSBoolean, getProperty("AutoEnable"));
    if (ab) autoMode = ab->isTrue();
    uint32_t manual = 0;
    if (PE_parse_boot_argn("-g814ampmanual", &manual, sizeof(manual))) autoMode = false;
    gAuto = autoMode ? 1 : 0;
    setProperty("auto-mode", autoMode);
    if (autoMode) {
        OSDictionary *m = serviceMatching("AppleHDAEngineOutput");
        if (m) { hdaNotifier = addMatchingNotification(gIOFirstPublishNotification, m, hdaPublished, this); m->release(); }
        if (!hdaNotifier) { autoMode = false; gAuto = 0; logf("auto: no notification for AppleHDAEngineOutput: manual mode"); }
    }

    logf("start: GSPI3 %08x at %s, sysctl debug.g814amp.cmd (0 survey, 1 probe, 2 configure, 3 enable, 9 all), auto mode %d", id, provider->getName(), autoMode);
    setStatus(autoMode ? "loaded; auto mode: waiting for AppleHDAEngineOutput" : "loaded; run sysctl debug.g814amp.cmd=0 for the survey");
    publishLog();
    registerService();
    return true;
}

// Auto mode -----------------------------------------------------------------------------------------------------
bool G814Amp::hdaPublished(void *target, void *, IOService *svc, IONotifier *) {
    G814Amp *me = (G814Amp *)target;
    if (!me || !me->lock || !svc) return true;
    IOLockLock(me->lock);
    if (!me->hdaEngine) {
        svc->retain();
        me->hdaEngine = svc;
        me->logf("auto: %s published; polling IOAudioEngineState every 1 s", svc->getName());
        me->publishLog();
        if (me->timer) me->timer->setTimeoutMS(1000);
    }
    IOLockUnlock(me->lock);
    return true;
}

bool G814Amp::engineRunning() {
    if (!hdaEngine) return false;
    OSObject *o = hdaEngine->copyProperty("IOAudioEngineState");
    OSNumber *n = OSDynamicCast(OSNumber, o);
    bool run = n && n->unsigned32BitValue() == 1;
    if (o) o->release();
    return run;
}

void G814Amp::autoTick() {
    bool run = engineRunning();
    bool on = enabled[0] && enabled[1];
    if (run && !on) {
        if (autoRetries >= 5) {
            if (!autoGaveUp) { autoGaveUp = true; setStatus("auto: gave up after %d failed enables (cmd=9 by hand)", autoRetries); publishLog(); }
            return;
        }
        autoRetries++;
        logf("auto: output engine running: probe + configure + enable (try %d)", autoRetries);
        bool ok = mapAll() && sspInit() && ampProbeAll();
        for (int a = 0; ok && a < G814AMP_NAMPS; a++) ok = ampConfigure(a);
        for (int a = 0; ok && a < G814AMP_NAMPS; a++) ok = ampEnable(a);
        for (int a = 0; ok && a < G814AMP_NAMPS; a++) {
            // the PLL runs only with GLOBAL_EN on; live lock = RAW3 bit 1 (reads 0 with GLOBAL_EN off)
            if (!ampPoll(a, CS35L41_IRQ1_RAW_STATUS3, CS35L41_PLL_LOCK, CS35L41_PLL_LOCK, 100, 2)) {
                uint32_t raw3 = 0; ampRead(a, CS35L41_IRQ1_RAW_STATUS3, &raw3);
                logf("auto: amp%d PLL not locked after 200 ms (RAW3 %08x): no BCLK from the codec", a, raw3);
                ok = false;
            }
        }
        if (ok) {
            for (int a = 0; a < G814AMP_NAMPS; a++) ampUnmute(a);
            wantEnabled = true;
            autoRetries = 0;
            setStatus("auto: ENABLED both amps at gain code %d (output engine running, PLL locked)", gGain);
        } else {
            allOff(false);
            setStatus("auto: enable failed (try %d of 5, see log); amps off", autoRetries);
        }
        gState = (sspReady ? 1 : 0) | ((probed[0] && probed[1]) ? 2 : 0) | ((configured[0] && configured[1]) ? 4 : 0) |
                 ((enabled[0] && enabled[1]) ? 8 : 0) | (faultCount ? 0x100 : 0);
        publishLog();
    } else if (!run && on) {
        logf("auto: output engine stopped: muting, GLOBAL_EN off");
        for (int a = 0; a < G814AMP_NAMPS; a++) ampDisable(a);
        wantEnabled = false;
        gState &= ~8;
        setStatus("auto: amps off (output engine stopped)");
        publishLog();
    }
}

void G814Amp::stop(IOService *provider) {
    if (hdaNotifier) { hdaNotifier->remove(); hdaNotifier = nullptr; }
    IOLockLock(lock);
    if (timer) timer->cancelTimeout();
    if (hdaEngine) { hdaEngine->release(); hdaEngine = nullptr; }
    allOff(true);
    unmapAll();
    IOLockUnlock(lock);
    PMstop();
    for (size_t i = sizeof(gOids) / sizeof(gOids[0]); i-- > 0;) sysctl_unregister_oid(gOids[i]);
    gInstance = nullptr;
    if (timer) { if (wl) wl->removeEventSource(timer); timer->release(); timer = nullptr; }
    if (lock) { IOLockFree(lock); lock = nullptr; }
    super::stop(provider);
}

IOReturn G814Amp::setPowerState(unsigned long state, IOService *dev) {
    if (!lock) return kIOPMAckImplied;
    IOLockLock(lock);
    if (state == 0) {
        if (sspReady) {
            logf("sleep: amps off, reset low (wantEnabled=%d)", wantEnabled);
            if (timer) timer->cancelTimeout();
            allOff(true);
            unmapAll();          // the BAR / LPSS context is lost in D3; map again on wake
        }
    } else {
        if (autoMode && gAuto) {
            autoRetries = 0; autoGaveUp = false;
            logf("wake: auto mode re-armed (amps follow the output engine)");
            if (timer && hdaEngine) timer->setTimeoutMS(1000);
        } else if (wantEnabled && !enabled[0]) {
            logf("wake: re-running probe + configure + enable");
            bool ok = mapAll() && sspInit() && ampProbeAll();
            for (int a = 0; ok && a < G814AMP_NAMPS; a++) ok = ampConfigure(a);
            for (int a = 0; ok && a < G814AMP_NAMPS; a++) ok = ampEnable(a);
            if (ok) { for (int a = 0; a < G814AMP_NAMPS; a++) ampUnmute(a); if (timer) timer->setTimeoutMS(1000); }
            setStatus(ok ? "wake: amps re-enabled" : "wake: re-enable FAILED (see log)");
            publishLog();
        }
    }
    IOLockUnlock(lock);
    return kIOPMAckImplied;
}

void G814Amp::systemWillShutdown(IOOptionBits specifier) {
    if (lock) {
        IOLockLock(lock);
        if (timer) timer->cancelTimeout();
        if (sspReady) { logf("shutdown(%x): amps off, reset low", (unsigned)specifier); allOff(true); }
        IOLockUnlock(lock);
    }
    super::systemWillShutdown(specifier);
}

// ---------------------------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------------------------
int G814Amp::runCommand(int cmd) {
    if (!lock) return ENXIO;
    IOLockLock(lock);
    bool ok = true;
    logf("cmd %d (gain code %d)", cmd, gGain);
    switch (cmd) {
    case 0:
        ok = mapAll();
        if (ok) {
            logf("PCI cmd=%04x pmcs: see survey; BAR0 phys %llx CAPS=%08x RESETS=%08x CLOCK=%08x CS_CTRL=%08x GATE=%08x SSCR0=%08x SSCR1=%08x SSSR=%08x",
                 pci->configRead16(kIOPCIConfigCommand), barPhys, r32(bar, PRIV_CAPS), r32(bar, PRIV_RESETS), r32(bar, PRIV_CLOCK),
                 r32(bar, PRIV_CS_CTRL), r32(bar, PRIV_CLOCK_GATE), r32(bar, SSCR0), r32(bar, SSCR1), r32(bar, SSSR));
            padSurvey();
            setStatus("survey done; reset pad TX=%d cs1 TX=%d (see log)", padGetTx(COMM_S, PAD_RESET), padGetTx(COMM_D, PAD_CS1));
        } else setStatus("survey FAILED: mapping (see log)");
        break;
    case 1:
        ok = mapAll() && sspInit() && ampProbeAll();
        setStatus(ok ? "probe OK: amp0 DEVID %05x REV %02x OTP %x, amp1 DEVID %05x REV %02x OTP %x" : "probe FAILED (see log)",
                  devid[0], revid[0], otpid[0], devid[1], revid[1], otpid[1]);
        break;
    case 2:
        if (!probed[0] || !probed[1]) { ok = false; setStatus("configure refused: probe first (cmd=1)"); break; }
        for (int a = 0; ok && a < G814AMP_NAMPS; a++) ok = ampConfigure(a);
        setStatus(ok ? "configured both amps (muted, GLOBAL_EN off)" : "configure FAILED (see log)");
        break;
    case 3:
        if (!configured[0] || !configured[1]) { ok = false; setStatus("enable refused: configure first (cmd=2)"); break; }
        for (int a = 0; ok && a < G814AMP_NAMPS; a++) ok = ampEnable(a);
        if (ok) {
            for (int a = 0; a < G814AMP_NAMPS; a++) ampUnmute(a);
            wantEnabled = true;
            if (timer) timer->setTimeoutMS(1000);
            setStatus("ENABLED both amps at gain code %d; play audio now; cmd=4 for PLL lock / errors", gGain);
        } else { allOff(false); setStatus("enable FAILED: amps disabled again (see log)"); }
        break;
    case 4:
        if (!sspReady) { ok = false; setStatus("status refused: not probed"); break; }
        for (int a = 0; a < G814AMP_NAMPS; a++) ampStatus(a);
        setStatus("status dumped (see log)");
        break;
    case 5:
        wantEnabled = false;
        if (gAuto) { gAuto = 0; logf("auto mode paused by cmd 5 (sysctl debug.g814amp.auto=1 resumes)"); }
        if (timer) timer->cancelTimeout();
        allOff(false);
        setStatus("disabled (muted, GLOBAL_EN off, AMP_EN off); auto mode paused");
        break;
    case 6:
        wantEnabled = false;
        if (gAuto) { gAuto = 0; logf("auto mode paused by cmd 6 (sysctl debug.g814amp.auto=1 resumes)"); }
        if (timer) timer->cancelTimeout();
        allOff(true);
        setStatus("disabled and held in reset; auto mode paused");
        break;
    case 9:
        ok = mapAll() && sspInit() && ampProbeAll();
        for (int a = 0; ok && a < G814AMP_NAMPS; a++) ok = ampConfigure(a);
        for (int a = 0; ok && a < G814AMP_NAMPS; a++) ok = ampEnable(a);
        if (ok) {
            for (int a = 0; a < G814AMP_NAMPS; a++) ampUnmute(a);
            wantEnabled = true;
            if (timer) timer->setTimeoutMS(1000);
            setStatus("ENABLED both amps at gain code %d (DEVID %05x/%05x REV %02x/%02x); play audio now", gGain, devid[0], devid[1], revid[0], revid[1]);
        } else { allOff(false); setStatus("go FAILED: amps disabled again (see log)"); }
        break;
    default:
        ok = false;
        setStatus("unknown cmd %d", cmd);
    }
    gState = (sspReady ? 1 : 0) | ((probed[0] && probed[1]) ? 2 : 0) | ((configured[0] && configured[1]) ? 4 : 0) |
             ((enabled[0] && enabled[1]) ? 8 : 0) | (faultCount ? 0x100 : 0);
    publishLog();
    IOLockUnlock(lock);
    return ok ? 0 : EIO;
}

// ---------------------------------------------------------------------------------------------------------------
// PCI, mapping, LPSS, SSP
// ---------------------------------------------------------------------------------------------------------------
bool G814Amp::pciWake() {
    uint8_t pmOff = 0;
    if (pci->findPCICapability(kIOPCIPowerManagementCapability, &pmOff) && pmOff) {
        uint16_t pmcs = pci->configRead16(pmOff + 4);
        logf("PCI PM cap at %02x PMCS=%04x (D%d)", pmOff, pmcs, pmcs & 3);
        if (pmcs & 3) {
            pci->configWrite16(pmOff + 4, pmcs & ~3u);
            IOSleep(10);
            logf("PCI PMCS now %04x", pci->configRead16(pmOff + 4));
        }
    } else logf("PCI: no PM capability found");
    pci->setMemoryEnable(true);
    uint16_t cmd = pci->configRead16(kIOPCIConfigCommand);
    if (!(cmd & 2)) { logf("PCI: memory space still disabled (cmd %04x)", cmd); return false; }
    return true;
}

bool G814Amp::mapAll() {
    if (bar && gpio[0] && gpio[1] && gpio[2]) return true;
    if (!pciWake()) return false;
    if (!barMap) {
        barMap = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
        if (!barMap) { logf("BAR0 map failed"); return false; }
        bar = (volatile uint8_t *)barMap->getVirtualAddress();
        barPhys = barMap->getPhysicalAddress();
        uint32_t caps = r32(bar, PRIV_CAPS);
        logf("BAR0 virt mapped, phys %llx len %llx, CAPS %08x (type %u, native CS count field %u)", barPhys,
             (uint64_t)barMap->getLength(), caps, (caps >> 4) & 0xF, (caps >> 9) & 0xF);
        if (caps == 0xFFFFFFFFu) { logf("BAR0 reads all ones: device not powered"); return false; }
    }
    for (int i = 0; i < 3; i++) {
        if (gpio[i]) continue;
        gpioMD[i] = IOMemoryDescriptor::withPhysicalAddress(kGpioCommPhys[i], 0x1000, kIODirectionInOut);
        if (!gpioMD[i]) { logf("GPIO community %d descriptor failed", i); return false; }
        gpioMap[i] = gpioMD[i]->map(kIOMapInhibitCache);
        if (!gpioMap[i]) { logf("GPIO community %d map failed", i); return false; }
        gpio[i] = (volatile uint8_t *)gpioMap[i]->getVirtualAddress();
    }
    return true;
}

void G814Amp::unmapAll() {
    sspReady = false;
    for (int a = 0; a < G814AMP_NAMPS; a++) probed[a] = configured[a] = enabled[a] = false;
    if (barMap) { barMap->release(); barMap = nullptr; bar = nullptr; }
    for (int i = 0; i < 3; i++) {
        if (gpioMap[i]) { gpioMap[i]->release(); gpioMap[i] = nullptr; }
        if (gpioMD[i]) { gpioMD[i]->release(); gpioMD[i] = nullptr; }
        gpio[i] = nullptr;
    }
}

bool G814Amp::sspInit() {
    if (sspReady) return true;
    // intel_lpss_init_dev: reset, release, remap address; clock to unity (100 MHz) and enabled; dynamic gating off.
    w32(bar, PRIV_RESETS, 0);
    IODelay(10);
    w32(bar, PRIV_RESETS, 0x7);
    w32(bar, PRIV_REMAP_ADDR, (uint32_t)barPhys);
    w32(bar, PRIV_REMAP_ADDR + 4, (uint32_t)(barPhys >> 32));
    uint32_t clk = r32(bar, PRIV_CLOCK);
    logf("LPSS CLOCK before %08x (en=%u M=%u N=%u)", clk, clk & 1, (clk >> 1) & 0x7FFF, (clk >> 16) & 0x7FFF);
    w32(bar, PRIV_CLOCK, (1u << 31) | (1u << 16) | (1u << 1) | 1u);   // update, N=1, M=1, enable
    w32(bar, PRIV_CLOCK_GATE, 0x3);                                   // clock force on (cs_clk_stays_gated quirk)
    // SSP: disabled while configuring. 8-bit Motorola SPI mode 0, SCR for 4 MHz from 100 MHz.
    uint32_t scr = (LPSS_CLK_HZ + SPI_HZ - 1) / SPI_HZ - 1;           // 24 -> 100/25 = 4.0 MHz
    w32(bar, SSCR0, 0);
    w32(bar, SSCR1, 0);                                               // SPO=0 SPH=0, no interrupts, no DMA
    w32(bar, SSTO, 0);
    w32(bar, SSITF, (31u << 8) | 55u);                                // thresholds (unused in PIO), Linux values 32/56
    w32(bar, SSIRF, 0);
    // software chip select, deasserted, CS0 selected
    uint32_t cs = r32(bar, PRIV_CS_CTRL);
    cs = (cs & ~(3u << 8)) | CS_CTRL_SW_MODE | CS_CTRL_CS_HIGH;
    w32(bar, PRIV_CS_CTRL, cs);
    w32(bar, SSCR0, (scr << 8) | SSCR0_DSS8);
    w32(bar, SSCR0, (scr << 8) | SSCR0_DSS8 | SSCR0_SSE);
    w32(bar, SSSR, SSSR_ROR);                                         // clear a stale overrun
    while (r32(bar, SSSR) & SSSR_RNE) (void)r32(bar, SSDR);           // drain
    // amp #1 deselected (GPP_D20 high) before any native-CS transfer (Linux cs35l41_hda_property.c)
    if (!padSetTx(COMM_D, PAD_CS1, true)) { logf("cannot drive GPP_D20 (CS of amp 1) high"); return false; }
    logf("SSP ready: SSCR0=%08x (SCR %u -> %u Hz) SSSR=%08x CS_CTRL=%08x CLOCK=%08x", r32(bar, SSCR0), scr,
         LPSS_CLK_HZ / (scr + 1), r32(bar, SSSR), r32(bar, PRIV_CS_CTRL), r32(bar, PRIV_CLOCK));
    sspReady = true;
    return true;
}

void G814Amp::csSet(int amp, bool assert) {
    if (amp == 0) {
        uint32_t cs = r32(bar, PRIV_CS_CTRL) & ~(3u << 8);          // CS0
        cs |= CS_CTRL_SW_MODE;
        if (assert) cs &= ~CS_CTRL_CS_HIGH; else cs |= CS_CTRL_CS_HIGH;
        w32(bar, PRIV_CS_CTRL, cs);
    } else {
        padSetTx(COMM_D, PAD_CS1, !assert);                           // active low
    }
    IODelay(2);
}

bool G814Amp::sspXfer(int amp, const uint8_t *tx, uint8_t *rx, size_t n) {
    if (!sspReady) return false;
    while (r32(bar, SSSR) & SSSR_RNE) (void)r32(bar, SSDR);
    w32(bar, SSSR, SSSR_ROR);
    csSet(amp, true);
    bool ok = true;
    for (size_t i = 0; i < n && ok; i++) {
        int t = 0;
        while (!(r32(bar, SSSR) & SSSR_TNF) && t++ < 100000) IODelay(1);
        if (t >= 100000) { logf("SSP TX FIFO never empty, SSSR=%08x", r32(bar, SSSR)); ok = false; break; }
        w32(bar, SSDR, tx[i]);
        t = 0;
        while (!(r32(bar, SSSR) & SSSR_RNE) && t++ < 100000) IODelay(1);
        if (t >= 100000) { logf("SSP RX timeout at byte %zu, SSSR=%08x", i, r32(bar, SSSR)); ok = false; break; }
        uint8_t v = (uint8_t)r32(bar, SSDR);
        if (rx) rx[i] = v;
    }
    int t = 0;
    while ((r32(bar, SSSR) & SSSR_BSY) && t++ < 100000) IODelay(1);
    csSet(amp, false);
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------
// GPIO pads
// ---------------------------------------------------------------------------------------------------------------
bool G814Amp::padSetTx(int comm, uint32_t off, bool high) {
    uint32_t v = r32(gpio[comm], off);
    if (v & PADCFG0_PMODE_MASK) { logf("pad %d/%03x in native mode (PADCFG0 %08x): refusing to drive", comm, off, v); return false; }
    uint32_t nv = (v & ~(PADCFG0_GPIOTXSTATE | PADCFG0_GPIOTXDIS)) | (high ? PADCFG0_GPIOTXSTATE : 0);
    if (nv == v) return true;
    w32(gpio[comm], off, nv);
    uint32_t rb = r32(gpio[comm], off);
    if ((rb & PADCFG0_GPIOTXSTATE) != (nv & PADCFG0_GPIOTXSTATE)) { logf("pad %d/%03x write did not take (%08x -> %08x, read %08x): locked?", comm, off, v, nv, rb); return false; }
    return true;
}

bool G814Amp::padGetTx(int comm, uint32_t off) {
    return gpio[comm] ? (r32(gpio[comm], off) & PADCFG0_GPIOTXSTATE) != 0 : false;
}

void G814Amp::padSurvey() {
    struct { const char *name; int comm; uint32_t off; int group; int bit; } pads[] = {
        { "GPP_S0 reset", COMM_S, PAD_RESET, 0, 0 }, { "GPP_S1 irq", COMM_S, PAD_IRQ, 0, 1 },
        { "GPP_D20 cs1", COMM_D, PAD_CS1, 0, 20 },   { "GPP_G0 spkid", COMM_G, PAD_SPKID, 1, 0 },
    };
    for (auto &p : pads) {
        uint32_t c0 = r32(gpio[p.comm], p.off), c1 = r32(gpio[p.comm], p.off + PADCFG1_OFF);
        uint32_t lock = r32(gpio[p.comm], PADCFGLOCK + p.group * 8), locktx = r32(gpio[p.comm], PADCFGLOCK + p.group * 8 + 4);
        uint32_t host = r32(gpio[p.comm], HOSTSW_OWN + p.group * 4);
        logf("%s PADCFG0=%08x PADCFG1=%08x pmode=%u txdis=%u rxdis=%u tx=%u rx=%u lock=%u locktx=%u hostsw(gpio)=%u", p.name, c0, c1,
             (c0 >> 10) & 3, (c0 >> 8) & 1, (c0 >> 9) & 1, c0 & 1, (c0 >> 1) & 1, (lock >> p.bit) & 1, (locktx >> p.bit) & 1, (host >> p.bit) & 1);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// CS35L41 register access (regmap-spi: 32-bit reg BE, 16 pad bits, 32-bit value BE, read flag 0x80)
// ---------------------------------------------------------------------------------------------------------------
bool G814Amp::ampWrite(int amp, uint32_t reg, uint32_t val) {
    uint8_t tx[10] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg, 0, 0,
                       (uint8_t)(val >> 24), (uint8_t)(val >> 16), (uint8_t)(val >> 8), (uint8_t)val };
    return sspXfer(amp, tx, nullptr, sizeof(tx));
}

bool G814Amp::ampRead(int amp, uint32_t reg, uint32_t *val) {
    uint32_t r = reg | 0x80000000u;
    uint8_t tx[10] = { (uint8_t)(r >> 24), (uint8_t)(r >> 16), (uint8_t)(r >> 8), (uint8_t)r, 0, 0, 0, 0, 0, 0 };
    uint8_t rx[10] = {};
    if (!sspXfer(amp, tx, rx, sizeof(tx))) return false;
    *val = ((uint32_t)rx[6] << 24) | ((uint32_t)rx[7] << 16) | ((uint32_t)rx[8] << 8) | rx[9];
    return true;
}

bool G814Amp::ampUpdate(int amp, uint32_t reg, uint32_t mask, uint32_t val) {
    uint32_t cur;
    if (!ampRead(amp, reg, &cur)) return false;
    uint32_t nv = (cur & ~mask) | (val & mask);
    if (nv == cur) return true;
    return ampWrite(amp, reg, nv);
}

bool G814Amp::ampPoll(int amp, uint32_t reg, uint32_t mask, uint32_t want, int tries, int ms) {
    uint32_t v = 0;
    for (int i = 0; i < tries; i++) {
        if (!ampRead(amp, reg, &v)) return false;
        if ((v & mask) == want) return true;
        IOSleep(ms);
    }
    logf("amp%d poll %08x & %08x != %08x timed out (last %08x)", amp, reg, mask, want, v);
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// CS35L41 sequences (Linux cs35l41_hda_probe / cs35l41-lib.c)
// ---------------------------------------------------------------------------------------------------------------
bool G814Amp::ampProbeAll() {
    // shared reset: low 2 ms, high, 2 ms (both amps)
    logf("reset pad GPP_S0 was TX=%d; pulsing low -> high", padGetTx(COMM_S, PAD_RESET));
    if (!padSetTx(COMM_S, PAD_RESET, false)) return false;
    IOSleep(2);
    if (!padSetTx(COMM_S, PAD_RESET, true)) return false;
    IOSleep(2);
    bool ok = true;
    for (int a = 0; a < G814AMP_NAMPS; a++) ok = ampProbe(a) && ok;
    return ok;
}

bool G814Amp::ampProbe(int amp) {
    probed[amp] = configured[amp] = enabled[amp] = false;
    if (!ampWrite(amp, CS35L41_SFT_RESET, CS35L41_SOFTWARE_RESET)) return false;
    IOSleep(2);
    if (!ampPoll(amp, CS35L41_IRQ1_STATUS4, CS35L41_OTP_BOOT_DONE, CS35L41_OTP_BOOT_DONE, 100, 1)) { logf("amp%d: OTP boot not done", amp); return false; }
    uint32_t st3 = 0;
    if (!ampRead(amp, CS35L41_IRQ1_STATUS3, &st3)) return false;
    if (st3 & CS35L41_OTP_BOOT_ERR) { logf("amp%d: OTP boot error (STATUS3 %08x)", amp, st3); return false; }
    if (!ampRead(amp, CS35L41_DEVID, &devid[amp]) || !ampRead(amp, CS35L41_REVID, &revid[amp]) || !ampRead(amp, CS35L41_OTPID, &otpid[amp])) return false;
    uint32_t want = ((revid[amp] & CS35L41_MTLREVID_MASK) % 2) ? CS35L41R_CHIP_ID : CS35L41_CHIP_ID;
    logf("amp%d: DEVID %08x REVID %08x OTPID %08x (want DEVID %05x)", amp, devid[amp], revid[amp], otpid[amp], want);
    if (devid[amp] != want) { logf("amp%d: wrong DEVID", amp); return false; }
    probed[amp] = true;
    return true;
}

bool G814Amp::ampErrata(int amp) {
    struct RV { uint32_t r, v; };
    static const RV a0[] = { { 0x00003854, 0x05180240 }, { CS35L41_VIMON_SPKMON_RESYNC, 0 }, { CS35L41_UNDOC_4310, 0 }, { CS35L41_VPVBST_FS_SEL, 0 },
                             { CS35L41_OTP_TRIM_30, 0x9091A1C8 }, { 0x00003014, 0x0200EE0E }, { CS35L41_BSTCVRT_DCM_CTRL, 0x51 }, { 0x00000054, 0x4 },
                             { CS35L41_IRQ1_DB3, 0 }, { CS35L41_IRQ2_DB3, 0 }, { CS35L41_DSP1_YM_ACCEL_PL0_PRI, 0 }, { CS35L41_DSP1_XM_ACCEL_PL0_PRI, 0 },
                             { CS35L41_PWR_CTRL2, 0 }, { CS35L41_AMP_GAIN_CTRL, 0 }, { CS35L41_ASP_TX3_SRC, 0 }, { CS35L41_ASP_TX4_SRC, 0 } };
    static const RV b0b2[] = { { CS35L41_VIMON_SPKMON_RESYNC, 0 }, { CS35L41_UNDOC_4310, 0 }, { CS35L41_VPVBST_FS_SEL, 0 }, { CS35L41_BSTCVRT_DCM_CTRL, 0x51 },
                               { CS35L41_DSP1_YM_ACCEL_PL0_PRI, 0 }, { CS35L41_DSP1_XM_ACCEL_PL0_PRI, 0 }, { CS35L41_PWR_CTRL2, 0 }, { CS35L41_AMP_GAIN_CTRL, 0 },
                               { CS35L41_ASP_TX3_SRC, 0 }, { CS35L41_ASP_TX4_SRC, 0 } };
    const RV *seq; size_t n;
    switch (revid[amp]) {
    case CS35L41_REVID_A0: seq = a0; n = sizeof(a0) / sizeof(a0[0]); break;
    case CS35L41_REVID_B0: case CS35L41_REVID_B2: seq = b0b2; n = sizeof(b0b2) / sizeof(b0b2[0]); break;
    default: logf("amp%d: unknown REVID %02x, no errata known", amp, revid[amp]); return false;
    }
    for (size_t i = 0; i < n; i++) if (!ampWrite(amp, seq[i].r, seq[i].v)) return false;
    return ampWrite(amp, CS35L41_DSP1_CCM_CORE_CTRL, 0);
}

// Must run with the test key unlocked (cs35l41_otp_unpack, verbatim logic)
bool G814Amp::ampOtpUnpack(int amp) {
    const otp_elem *map; uint32_t num;
    switch (otpid[amp]) {
    case 0x01: case 0x08: map = otp_map_1; num = sizeof(otp_map_1) / sizeof(otp_map_1[0]); break;
    case 0x02: case 0x03: case 0x06: map = otp_map_2; num = sizeof(otp_map_2) / sizeof(otp_map_2[0]); break;
    default: logf("amp%d: OTP map for id %u unknown", amp, otpid[amp]); return false;
    }
    uint32_t mem[CS35L41_OTP_SIZE_WORDS];
    for (uint32_t i = 0; i < CS35L41_OTP_SIZE_WORDS; i++) if (!ampRead(amp, CS35L41_OTP_MEM0 + 4 * i, &mem[i])) return false;
    int bit_offset = 16, word_offset = 2;
    uint32_t written = 0;
    for (uint32_t i = 0; i < num; i++) {
        uint32_t otp_val;
        const int size = map[i].size;
        if (bit_offset + size - 1 >= 32) {
            otp_val = (mem[word_offset] & GENMASK(31, bit_offset)) >> bit_offset;
            word_offset++;
            otp_val |= (mem[word_offset] & GENMASK(bit_offset + size - 33, 0)) << (32 - bit_offset);
            bit_offset += size - 32;
        } else if (bit_offset + size - 1 >= 0) {
            otp_val = (mem[word_offset] & GENMASK(bit_offset + size - 1, bit_offset)) >> bit_offset;
            bit_offset += size;
        } else otp_val = 0;
        if (bit_offset == 32) { bit_offset = 0; word_offset++; }
        if (map[i].reg != 0) {
            if (!ampUpdate(amp, map[i].reg, GENMASK(map[i].shift + size - 1, map[i].shift), otp_val << map[i].shift)) return false;
            written++;
        }
    }
    logf("amp%d: OTP id %u unpacked, %u trim fields applied", amp, otpid[amp], written);
    return true;
}

// Linux row for 10431CCF: internal boost, L_BST 1.0 uH, C_BST 24 uF, I_PK 4500 mA
bool G814Amp::ampBoostConfig(int amp) {
    const uint32_t lbst = 0 /* 1.0 uH */, crange = 1 /* 20..50 uF */;
    static const uint8_t k1[4][5] = { { 0x24, 0x32, 0x32, 0x4F, 0x57 }, { 0x24, 0x32, 0x32, 0x4F, 0x57 }, { 0x40, 0x32, 0x32, 0x4F, 0x57 }, { 0x40, 0x32, 0x32, 0x4F, 0x57 } };
    static const uint8_t k2[4][5] = { { 0x24, 0x49, 0x66, 0xA3, 0xEA }, { 0x24, 0x49, 0x66, 0xA3, 0xEA }, { 0x48, 0x49, 0x66, 0xA3, 0xEA }, { 0x48, 0x49, 0x66, 0xA3, 0xEA } };
    static const uint8_t slope[4] = { 0x75, 0x6B, 0x3B, 0x28 };
    const uint32_t ipk = ((4500 - 1600) / 50) + 0x10;   // 0x4A
    if (!ampUpdate(amp, CS35L41_BSTCVRT_COEFF, CS35L41_BST_K1_MASK | CS35L41_BST_K2_MASK,
                   ((uint32_t)k1[lbst][crange] << CS35L41_BST_K1_SHIFT) | ((uint32_t)k2[lbst][crange] << CS35L41_BST_K2_SHIFT))) return false;
    if (!ampUpdate(amp, CS35L41_BSTCVRT_SLOPE_LBST, CS35L41_BST_SLOPE_MASK | CS35L41_BST_LBST_VAL_MASK,
                   ((uint32_t)slope[lbst] << CS35L41_BST_SLOPE_SHIFT) | (lbst << CS35L41_BST_LBST_VAL_SHIFT))) return false;
    if (!ampUpdate(amp, CS35L41_BSTCVRT_PEAK_CUR, CS35L41_BST_IPK_MASK, ipk << CS35L41_BST_IPK_SHIFT)) return false;
    return ampUpdate(amp, CS35L41_PWR_CTRL2, CS35L41_BST_EN_MASK, CS35L41_BST_EN_DEFAULT << CS35L41_BST_EN_SHIFT);
}

bool G814Amp::ampConfigure(int amp) {
    configured[amp] = false;
    if (!probed[amp]) return false;
    // test key unlock, errata, OTP trims, lock
    if (!ampWrite(amp, CS35L41_TEST_KEY_CTL, 0x55) || !ampWrite(amp, CS35L41_TEST_KEY_CTL, 0xAA)) return false;
    bool ok = ampErrata(amp) && ampOtpUnpack(amp);
    if (!ampWrite(amp, CS35L41_TEST_KEY_CTL, 0xCC) || !ampWrite(amp, CS35L41_TEST_KEY_CTL, 0x33)) return false;
    if (!ok) { logf("amp%d: errata/OTP failed", amp); return false; }
    // mute (Linux cs35l41_hda_mute)
    if (!ampMute(amp)) return false;
    // boost, GPIO pads (gpio1 unused hi-Z input, gpio2 = INTb open drain, input), IRQ unmask of the error bits
    if (!ampBoostConfig(amp)) return false;
    if (!ampUpdate(amp, CS35L41_GPIO1_CTRL1, CS35L41_GPIO_POL_MASK | CS35L41_GPIO_DIR_MASK, 1u << CS35L41_GPIO_DIR_SHIFT)) return false;
    if (!ampUpdate(amp, CS35L41_GPIO2_CTRL1, CS35L41_GPIO_POL_MASK | CS35L41_GPIO_DIR_MASK, 1u << CS35L41_GPIO_DIR_SHIFT)) return false;
    if (!ampUpdate(amp, CS35L41_GPIO_PAD_CONTROL, CS35L41_GPIO1_CTRL_MASK | CS35L41_GPIO2_CTRL_MASK,
                   (0u << CS35L41_GPIO1_CTRL_SHIFT) | (CS35L41_GPIO2_INT_OPEN_DRAIN << CS35L41_GPIO2_CTRL_SHIFT))) return false;
    const uint32_t errs = CS35L41_BST_OVP_ERR | CS35L41_BST_DCM_UVP_ERR | CS35L41_BST_SHORT_ERR | CS35L41_TEMP_WARN | CS35L41_TEMP_ERR | CS35L41_AMP_SHORT_ERR;
    if (!ampWrite(amp, CS35L41_IRQ1_STATUS1, 0xFFFFFFFFu)) return false;                       // clear stale status
    if (!ampUpdate(amp, CS35L41_IRQ1_MASK1, errs, 0)) return false;                              // unmask the 6 errors
    // channel map: amp 0 = left = RX slot 0, amp 1 = right = RX slot 1
    if (!ampUpdate(amp, CS35L41_SP_FRAME_RX_SLOT, 0x3F, (uint32_t)amp)) return false;
    // ASP / clocks / routing: Linux cs35l41_hda_config + _no_dsp + update_mixer (DSP bypass)
    struct RV { uint32_t r, v; };
    static const RV cfg[] = {
        { CS35L41_PLL_CLK_CTRL, 0x00000430 },   // PLL ref = BCLK 3.072 MHz, REFCLK_EN
        { CS35L41_DSP_CLK_CTRL, 0x00000003 },
        { CS35L41_GLOBAL_CLK_CTRL, 0x00000003 },// GLOBAL_FS 48 kHz
        { CS35L41_SP_RATE_CTRL, 0x00000021 },   // ASP BCLK 3.072 MHz
        { CS35L41_SP_FORMAT, 0x20200200 },      // 32-bit RX/TX slots, I2S, clock consumer
        { CS35L41_SP_TX_WL, 0x18 }, { CS35L41_SP_RX_WL, 0x18 },
        { CS35L41_ASP_TX1_SRC, 0x18 }, { CS35L41_ASP_TX2_SRC, 0x19 }, { CS35L41_DSP1_RX3_SRC, 0x18 }, { CS35L41_DSP1_RX4_SRC, 0x19 },
        { CS35L41_SP_HIZ_CTRL, 0x2 }, { CS35L41_DAC_PCM1_SRC, 0x08 }, { CS35L41_ASP_TX3_SRC, 0 }, { CS35L41_ASP_TX4_SRC, 0 },
        { CS35L41_DSP1_RX5_SRC, 0x20 }, { CS35L41_DSP1_RX6_SRC, 0x21 },
        { CS35L41_SP_ENABLES, CS35L41_ASP_RX1_EN_MASK },
        { CS35L41_DSP1_RX1_SRC, 0x08 }, { CS35L41_DSP1_RX2_SRC, 0x08 },
    };
    for (auto &e : cfg) if (!ampWrite(amp, e.r, e.v)) return false;
    uint32_t p1 = 0, p2 = 0;
    ampRead(amp, CS35L41_PWR_CTRL1, &p1); ampRead(amp, CS35L41_PWR_CTRL2, &p2);
    logf("amp%d: configured (PWR_CTRL1 %08x PWR_CTRL2 %08x)", amp, p1, p2);
    configured[amp] = true;
    return true;
}

bool G814Amp::ampMute(int amp) {
    return ampWrite(amp, CS35L41_AMP_GAIN_CTRL, 0) && ampWrite(amp, CS35L41_AMP_DIG_VOL_CTRL, 0x0000A678);
}

bool G814Amp::ampUnmute(int amp) {
    int g = gGain; if (g < 0) g = 0; if (g > 9) g = 9;
    uint32_t gain = ((uint32_t)g << CS35L41_AMP_GAIN_PCM_SHIFT) | 0x04;   // 4 -> 0x84 as Linux (PDM field unused)
    return ampWrite(amp, CS35L41_AMP_DIG_VOL_CTRL, 0x00008000) && ampWrite(amp, CS35L41_AMP_GAIN_CTRL, gain);
}

bool G814Amp::ampEnable(int amp) {
    if (!configured[amp]) return false;
    if (!ampUpdate(amp, CS35L41_PWR_CTRL2, CS35L41_AMP_EN_MASK, 1u << CS35L41_AMP_EN_SHIFT)) return false;
    if (!ampUpdate(amp, CS35L41_PWR_CTRL1, CS35L41_GLOBAL_EN_MASK, 1u << CS35L41_GLOBAL_EN_SHIFT)) return false;
    bool pup = ampPoll(amp, CS35L41_IRQ1_STATUS1, CS35L41_PUP_DONE_MASK, CS35L41_PUP_DONE_MASK, 100, 1);
    ampWrite(amp, CS35L41_IRQ1_STATUS1, CS35L41_PUP_DONE_MASK);
    uint32_t st1 = 0, err = ampErrors(amp, &st1);
    logf("amp%d: GLOBAL_EN set, PUP_DONE %s, IRQ1_STATUS1 %08x errors %08x", amp, pup ? "yes" : "NO", st1, err);
    if (!pup || err) { ampDisable(amp); return false; }
    enabled[amp] = true;
    return true;
}

uint32_t G814Amp::ampErrors(int amp, uint32_t *status1) {
    uint32_t st = 0;
    if (!ampRead(amp, CS35L41_IRQ1_STATUS1, &st)) return 0xFFFFFFFFu;
    if (status1) *status1 = st;
    return st & (CS35L41_BST_OVP_ERR | CS35L41_BST_DCM_UVP_ERR | CS35L41_BST_SHORT_ERR | CS35L41_TEMP_WARN | CS35L41_TEMP_ERR | CS35L41_AMP_SHORT_ERR);
}

void G814Amp::ampIrqRelease(int amp) {
    const uint32_t rls = CS35L41_AMP_SHORT_ERR_RLS | CS35L41_BST_SHORT_ERR_RLS | CS35L41_BST_OVP_ERR_RLS | CS35L41_BST_UVP_ERR_RLS |
                         CS35L41_TEMP_WARN_ERR_RLS | CS35L41_TEMP_ERR_RLS;
    ampWrite(amp, CS35L41_PROTECT_REL_ERR_IGN, 0);
    ampWrite(amp, CS35L41_PROTECT_REL_ERR_IGN, rls);
    ampWrite(amp, CS35L41_PROTECT_REL_ERR_IGN, 0);
}

bool G814Amp::ampDisable(int amp) {
    enabled[amp] = false;
    if (!sspReady) return false;
    ampMute(amp);
    uint32_t p1 = 0;
    if (ampRead(amp, CS35L41_PWR_CTRL1, &p1) && (p1 & CS35L41_GLOBAL_EN_MASK)) {
        ampUpdate(amp, CS35L41_PWR_CTRL1, CS35L41_GLOBAL_EN_MASK, 0);
        ampPoll(amp, CS35L41_IRQ1_STATUS1, CS35L41_PDN_DONE_MASK, CS35L41_PDN_DONE_MASK, 100, 1);
        ampWrite(amp, CS35L41_IRQ1_STATUS1, CS35L41_PDN_DONE_MASK);
    }
    ampUpdate(amp, CS35L41_PWR_CTRL2, CS35L41_AMP_EN_MASK, 0);
    ampIrqRelease(amp);
    return true;
}

void G814Amp::ampStatus(int amp) {
    uint32_t v[12] = {};
    static const uint32_t regs[12] = { CS35L41_PWR_CTRL1, CS35L41_PWR_CTRL2, CS35L41_IRQ1_STATUS1, CS35L41_IRQ1_STATUS2, CS35L41_IRQ1_STATUS3, CS35L41_IRQ1_STATUS4,
                                       CS35L41_IRQ1_RAW_STATUS1, CS35L41_IRQ1_RAW_STATUS3, CS35L41_AMP_GAIN_CTRL, CS35L41_AMP_DIG_VOL_CTRL, CS35L41_SP_FRAME_RX_SLOT, CS35L41_DAC_PCM1_SRC };
    for (int i = 0; i < 12; i++) ampRead(amp, regs[i], &v[i]);
    logf("amp%d: PWR1 %08x PWR2 %08x ST1 %08x ST2 %08x ST3 %08x (PLL_LOCK %u) ST4 %08x RAW1 %08x RAW3 %08x GAIN %08x VOL %08x RXSLOT %08x DACSRC %08x",
         amp, v[0], v[1], v[2], v[3], v[4], (v[4] >> 1) & 1, v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
    // ST3 bit1 is the latched PLL lock event (sticky); RAW3 bit1 is the live PLL_LOCK state.
}

void G814Amp::allOff(bool resetLow) {
    if (sspReady) for (int a = 0; a < G814AMP_NAMPS; a++) ampDisable(a);
    if (resetLow && gpio[COMM_S]) {
        padSetTx(COMM_S, PAD_RESET, false);
        for (int a = 0; a < G814AMP_NAMPS; a++) probed[a] = configured[a] = enabled[a] = false;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Fault poll (1 s): on any hardware error, mute and disable; no retry loops.
// ---------------------------------------------------------------------------------------------------------------
void G814Amp::timerFired(OSObject *owner, IOTimerEventSource *) {
    G814Amp *me = OSDynamicCast(G814Amp, owner);
    if (me) me->pollFaults();
}

void G814Amp::pollFaults() {
    if (!lock) return;
    IOLockLock(lock);
    bool again = false;
    bool autoLive = autoMode && gAuto && hdaEngine && !autoGaveUp;
    if (autoLive) { autoTick(); again = true; }
    if (sspReady) {
        for (int a = 0; a < G814AMP_NAMPS; a++) {
            if (!enabled[a]) continue;
            uint32_t st1 = 0, err = ampErrors(a, &st1);
            if (err == 0xFFFFFFFFu) { logf("amp%d: fault poll read failed", a); continue; }
            if (err) {
                faultCount++;
                logf("amp%d: FAULT IRQ1_STATUS1 %08x (OVP %u UVP %u BSTSHORT %u TEMPWARN %u TEMPERR %u AMPSHORT %u): muting and disabling", a, st1,
                     !!(err & CS35L41_BST_OVP_ERR), !!(err & CS35L41_BST_DCM_UVP_ERR), !!(err & CS35L41_BST_SHORT_ERR), !!(err & CS35L41_TEMP_WARN),
                     !!(err & CS35L41_TEMP_ERR), !!(err & CS35L41_AMP_SHORT_ERR));
                ampDisable(a);
                wantEnabled = false;
                setStatus("FAULT on amp%d: %08x, amps disabled (cmd=4 for detail)", a, err);
                gState |= 0x100;
                publishLog();
            } else again = true;
        }
    }
    if (again && timer) timer->setTimeoutMS(1000);
    IOLockUnlock(lock);
}
