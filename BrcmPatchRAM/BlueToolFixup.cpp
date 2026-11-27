//
//  BlueToolFixup.cpp
//  BrcmPatchRAM
//
//  Created by Dhinak G on 6/11/21.
//


#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_user.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/kern_version.hpp>
#include <Headers/kern_devinfo.hpp>
#include <kern/thread_call.h>
#include <libkern/OSAtomic.h>


#define MODULE_SHORT "btlfx"


class EXPORT BlueToolFixup : public IOService {
    OSDeclareDefaultStructors(BlueToolFixup)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
};

OSDefineMetaClassAndStructors(BlueToolFixup, IOService)


IOService *BlueToolFixup::probe(IOService *provider, SInt32 *score) {
    return ADDPR(startSuccess) ? IOService::probe(provider, score) : nullptr;
}



#pragma mark - Patches

static const uint8_t kSkipUpdateFilePathOriginal[] = "/etc/bluetool/SkipBluetoothAutomaticFirmwareUpdate";
static const uint8_t kSkipUpdateFilePathPatched[]  = "/System/Library/CoreServices/boot.efi";


// Workaround 12.4 Beta 3+ bug where macOS may detect the Bluetooth chipset twice
// Once as internal, and second as an external dongle:
// 'ERROR -- Third Party Dongle has the same address as the internal module'
// Mainly applicable for BCM2046 and BCM2070 chipsets (BT2.1)
static const uint8_t kSkipAddressCheckOriginal[] =
{
    0x48, 0x89, 0xF3,             // mov    rbx, rsi
    0xE8, 0x00, 0x00, 0x00, 0x00, // call   <somewhere>
    0x85, 0xC0,                   // test   eax, eax
    0x74, 0x1D,                   // je
};

static const uint8_t kSkipAddressCheckPatched[] =
{
    0x48, 0x89, 0xF3,             // mov        rbx, rsi
    0xE8, 0x00, 0x00, 0x00, 0x00, // call       <somewhere>
    0x85, 0xC0,                   // test       eax, eax
    0x72, 0x1D,                   // jb short
};

static const uint8_t kSkipAddressCheckMask[] =
{
    0xFF, 0xFF, 0xFF,
    0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF,
    0xFF, 0xFF,
};

static const uint8_t kVendorCheckOriginal[] =
{
    0x81, 0xFA,              // cmp edx
    0x5C, 0x0A, 0x00, 0x00,  // Vendor BRCM,
    0x74                     // jnz short
};

static const uint8_t kVendorCheckPatched[] =
{
    0x81, 0xFA,              // cmp edx
    0x5C, 0x0A, 0x00, 0x00,  // Vendor BRCM,
    0xEB                     // jmp short
};

// Workaround for bugged chipset range check that
// doesn't consider 0 "THIRD_PARTY_DONGLE" as valid.
// This patch allows bluetooth to turn back on after the first power cycle.
// See https://github.com/acidanthera/BrcmPatchRAM/pull/18 for more details.
static const uint8_t kBadChipsetCheckOriginal[] =
{
    0x81, 0xF9,              // cmp ecx
    0xCF, 0x07, 0x00, 0x00,  // int 1999
    0x72                     // jb short
};

static const uint8_t kBadChipsetCheckPatched[] =
{
    0x81, 0xF9,              // cmp ecx
    0xCF, 0x07, 0x00, 0x00,  // int 1999
    0xEB                     // jmp short
};

static const uint8_t kBadChipsetCheckOriginal13_3[] =
{
    0x81, 0xF9,              // cmp ecx
    0x9E, 0x0F, 0x00, 0x00,  // int 3998
    0x77, 0x1A               // ja short
};

static const uint8_t kBadChipsetCheckPatched13_3[] =
{
    0x90, 0x90,
    0x90, 0x90, 0x90, 0x90,
    0x90, 0x90
};

static const uint8_t kSkipInternalControllerNVRAMCheck13_3[] =
{
    0x41, 0x80, 0x00, 0x01, // xor     r15b, 1
    0x75, 0x00,             // jnz     short
    0x84, 0xDB,             // test    bl, bl
    0x75, 0x00              // jnz     short
};

static const uint8_t kSkipInternalControllerNVRAMCheckMask13_3[] =
{
    0xFF, 0xFF, 0x00, 0xFF,
    0xFF, 0x00,
    0xFF, 0xFF,
    0xFF, 0x00
};

static const uint8_t kSkipInternalControllerNVRAMCheckPatched13_3[] =
{
    0x90, 0x90, 0x90, 0x90,
    0x90, 0x90,
    0x90, 0x90,
    0x90, 0x90
};

static const uint8_t kBadChipsetCheckOriginal15_4[] =
{
    0x81, 0xF9,              // cmp ecx
    0x6F, 0x17, 0x00, 0x00,  // int 5999
    0x77, 0x30               // ja short
};

static const uint8_t kBadChipsetCheckPatched15_4[] =
{
    0x90, 0x90,
    0x90, 0x90, 0x90, 0x90,
    0x90, 0x90
};

// Optional universal workaround for bluetoothd Read Clock failures.
// Many third-party Bluetooth controllers (like Intel) fail Apple's HCI_Read_Clock check.
// When connecting to modern Apple headphones with W1/H1/H2 chips (e.g., AirPods Pro 2),
// this causes bluetoothd to abort A2DP audio bring-up in the Fast-Connect reconnect path.
// This results in the infamous "connected but no sound" issue for these devices.
// See: https://github.com/OpenIntelWireless/IntelBluetoothFirmware/issues/462
// We patch the Fast-Connect A2DP gate to unconditionally force the success path.
// Activated by boot-arg -btlfxa2dpcheck

// Apple FastConnect A2DP reconnect gate.
// Replaces "test r14d, r14d; je" with "xor r14d, r14d; jmp" to
// unconditionally take the audio-publish path.

// macOS 14.x/15.x/16.x (Sonoma/Sequoia/Tahoe)
static const uint8_t kFastConnectA2DPGateOriginal14_0[] =
{
    0x45, 0x85, 0xF6,
    0x74, 0x25,
    0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8B, 0x18
};

static const uint8_t kFastConnectA2DPGatePatched14_0[] =
{
    0x45, 0x31, 0xF6,
    0xEB, 0x25,
    0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8B, 0x18
};

static const uint8_t kFastConnectA2DPGateMask14_0[] =
{
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF
};

// macOS 12.x/13.x (Monterey/Ventura)
static const uint8_t kFastConnectA2DPGateOriginal12_0[] =
{
    0x85, 0xDB,
    0x74, 0x25,
    0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8B, 0x18
};

static const uint8_t kFastConnectA2DPGatePatched12_0[] =
{
    0x31, 0xDB,
    0xEB, 0x25,
    0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00,
    0x48, 0x8B, 0x18
};

static const uint8_t kFastConnectA2DPGateMask12_0[] =
{
    0xFF, 0xFF,
    0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF
};

// Optional: let bluetoothd use Bluetooth-spec (5.0) LE advertising sets on a THIRD_PARTY_DONGLE.
// bluetoothd decides "BT chip support for BT Spec advertisingsets" with a capability method
// (vtable +0x9c0 on 26.4.1) that only looks at the chipset enum, and chipset 0
// (THIRD_PARTY_DONGLE) is never in its list. With the flag off, LE_InitMultipleAdvertising
// falls back to the vendor (VSC) path, which also needs a non-zero chipset, so it returns
// 0x98 and LE advertising never starts ("Stack is not started"). That breaks
// AirDrop/Continuity discovery from iPhone.
// The patch replaces the capability call with "mov al, 1" at the only call site, so the
// rest (DisableSpecSets override, multi-advertising init) runs as Apple wrote it and sends
// standard HCI LE extended advertising commands, which BT 5.x controllers implement.
// Activated by boot-arg -btlfxadvsets

// macOS 26.4.1 (25E253) x86_64 bluetoothd, 0x10040e2ca
// mov rcx, [rax]; mov rdi, rax; call [rcx+0x9c0]; mov r14d, eax
static const uint8_t kSpecAdvSetsCapOriginal26[] =
{
    0x48, 0x8B, 0x08,
    0x48, 0x89, 0xC7,
    0xFF, 0x91, 0xC0, 0x09, 0x00, 0x00,
    0x41, 0x89, 0xC6
};

// mov rcx, [rax]; mov rdi, rax; mov eax, 1; nop; mov r14d, eax
// (mov eax, not mov al: the next instruction copies all of eax, and with "mov al, 1" its upper bits
// were the low bits of the object pointer in rax, so r14d was some pointer-derived value, not 1)
static const uint8_t kSpecAdvSetsCapPatched26[] =
{
    0x48, 0x8B, 0x08,
    0x48, 0x89, 0xC7,
    0xB8, 0x01, 0x00, 0x00, 0x00, 0x90,
    0x41, 0x89, 0xC6
};

// The extended scanning capability (vtable +0x9a8, 0x10040aae2 on 26.4.1). Apple enables it
// together with BT Spec advertising sets for every chipset it knows. Once the host has used
// LE extended advertising commands, the controller rejects legacy LE scan commands with
// Command Disallowed, so both must be on.
// mov rcx, [rax]; mov rdi, rax; call [rcx+0x9a8]; mov r14d, eax
static const uint8_t kExtScanCapOriginal26[] =
{
    0x48, 0x8B, 0x08,
    0x48, 0x89, 0xC7,
    0xFF, 0x91, 0xA8, 0x09, 0x00, 0x00,
    0x41, 0x89, 0xC6
};

// LE event mask (0x10017015c on 26.4.1): bluetoothd writes 0x1f (legacy LE events only) when
// the chipset is 0, so Extended Advertising Report, Advertising Set Terminated, etc. are
// masked and an extended scan never reports anything. Drop the cmove so the full mask
// (same as every Apple chipset) is used.
// test eax, eax; lea rax, [mask1f]; lea rdx, [maskFull]; cmove rdx, rax
static const uint8_t kLeEventMaskOriginal26[] =
{
    0x85, 0xC0,
    0x48, 0x8D, 0x05, 0xA0, 0xF2, 0x8F, 0x00,
    0x48, 0x8D, 0x15, 0xA1, 0xF2, 0x8F, 0x00,
    0x48, 0x0F, 0x44, 0xD0
};

static const uint8_t kLeEventMaskPatched26[] =
{
    0x85, 0xC0,
    0x48, 0x8D, 0x05, 0xA0, 0xF2, 0x8F, 0x00,
    0x48, 0x8D, 0x15, 0xA1, 0xF2, 0x8F, 0x00,
    0x0F, 0x1F, 0x40, 0x00
};

// Advertising set random address (0x10050ef2e on 26.4.1): for chipset <= 3 bluetoothd turns
// off LE Set Advertising Set Random Address, then enables sets whose own address type is
// random, which the controller rejects with Invalid HCI Command Parameters (0x12).
// Only skip that one call (jmp +7); the chipset == 0 block after it is left as is.
// cmp dword [rax+0x334], 3; ja +0x2a -> jmp +7
static const uint8_t kAdvSetAddrOriginal26[] =
{
    0x83, 0xB8, 0x34, 0x03, 0x00, 0x00, 0x03,
    0x77, 0x2A,
    0x31, 0xFF, 0xE8, 0xA2, 0x7D, 0xCE, 0xFF
};

static const uint8_t kAdvSetAddrPatched26[] =
{
    0x83, 0xB8, 0x34, 0x03, 0x00, 0x00, 0x03,
    0xEB, 0x07,
    0x31, 0xFF, 0xE8, 0xA2, 0x7D, 0xCE, 0xFF
};

// LE Extended Create Connection (0x10050eb5c on 26.4.1). With extended scanning on, the
// controller rejects legacy LE Create Connection (0x200d) with Command Disallowed, and after
// enough of those bluetoothd restarts itself ("Exceed max allowed consecutive command
// disallowed events"). Feed 1 into the setter of gEnableExtendedCreateConnectionCommand.
// movzx edi, byte [rbp-0xa2]; call ... -> mov edi, 1; nop2; call ...
static const uint8_t kExtCreateConnOriginal26[] =
{
    0x0F, 0xB6, 0xBD, 0x5E, 0xFF, 0xFF, 0xFF,
    0xE8, 0x2F, 0xB0, 0xCF, 0xFF
};

static const uint8_t kExtCreateConnPatched26[] =
{
    0xBF, 0x01, 0x00, 0x00, 0x00, 0x66, 0x90,
    0xE8, 0x2F, 0xB0, 0xCF, 0xFF
};

// Advertising set random address before every enable (0x1001f951b on 26.4.1). bluetoothd
// caches each set's random address and only sends LE Set Advertising Set Random Address
// when it changed. The Intel controller forgets the address when the set's parameters are
// rewritten (LE Set Extended Advertising Parameters with a random own address type), so
// the following enable fails with 0x12 over and over until the address rotates (in an
// HCI trace: 64693 failures without a fresh 0x2035, 0 with one). Always send it.
// mov edx, [rdx+rdi+3]; xor edx, [rax+rcx+0xd11]; or edx, r8d; je +0x80 -> nop6
static const uint8_t kAdvSetAddrResendOriginal26[] =
{
    0x8B, 0x54, 0x3A, 0x03,
    0x33, 0x94, 0x08, 0x11, 0x0D, 0x00, 0x00,
    0x44, 0x09, 0xC2,
    0x0F, 0x84, 0x80, 0x00, 0x00, 0x00
};

static const uint8_t kAdvSetAddrResendPatched26[] =
{
    0x8B, 0x54, 0x3A, 0x03,
    0x33, 0x94, 0x08, 0x11, 0x0D, 0x00, 0x00,
    0x44, 0x09, 0xC2,
    0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00
};

static bool shouldPatchBoardId = false;
static bool shouldPatchAddress = false;
static bool shouldPatchNvramCheck = false;
static bool shouldPatchA2DPCheck = false;
static bool shouldPatchSpecAdvSets = false;

// The advsets patches were made from bluetoothd of macOS 26.4.1 (25E253). Most of them contain
// rip-relative displacements and simply never match another build, but the two capability patches
// only match a vtable offset, which other builds use for other methods. They would then be applied
// without the rest: bluetoothd mixes LE extended and legacy commands, the controller answers with
// Command Disallowed, and bluetoothd restarts itself, dropping every connection.
//
// The build is checked against kern.osversion. launchd writes it from userspace, after Lilu's patcher
// callback has run (it is still empty then), but before any daemon starts. So the decision is taken
// when bluetoothd pages are validated, and cached once osversion is known.
static const char *kernelOsVersion {nullptr};
static bool specAdvSetsForced = false;
static volatile SInt32 specAdvSetsDecision = 0; // 0 undecided, 1 verified build, -1 other build

static void refuseSpecAdvSets();

static bool specAdvSetsAllowed() {
    if (specAdvSetsForced)
        return true;
    SInt32 decision = __atomic_load_n(&specAdvSetsDecision, __ATOMIC_ACQUIRE);
    if (decision)
        return decision > 0;
    // Not written yet: patch nothing now and decide on a later page
    if (!kernelOsVersion || !kernelOsVersion[0])
        return false;
    bool verified = !strncmp(kernelOsVersion, "25E253", sizeof("25E253"));
    SInt32 expected = 0;
    if (__atomic_compare_exchange_n(&specAdvSetsDecision, &expected, verified ? 1 : -1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && !verified)
        refuseSpecAdvSets();
    return verified;
}

static constexpr size_t kBoardIdSize = sizeof("Mac-F60DEB81FF30ACF6");
static constexpr size_t kBoardIdSizeLegacy = sizeof("Mac-F22586C8");

static char replacementBoardId[kBoardIdSize] {};

static const char boardIdsWithUSBBluetooth[][kBoardIdSize] = {
    "Mac-F60DEB81FF30ACF6",
    "Mac-9F18E312C5C2BF0B",
    "Mac-937CB26E2E02BB01",
    "Mac-E43C1C25D4880AD6",
    "Mac-06F11FD93F0323C5",
    "Mac-06F11F11946D27C5",
    "Mac-A369DDC4E67F1C45",
    "Mac-FFE5EF870D7BA81A",
    "Mac-DB15BD556843C820",
    "Mac-B809C3757DA9BB8D",
    "Mac-65CE76090165799A",
    "Mac-4B682C642B45593E",
    "Mac-77F17D7DA9285301",
    "Mac-BE088AF8C5EB4FA2"
};

static mach_vm_address_t orig_cs_validate {};

#pragma mark - Kernel patching code

#pragma mark - Patch statistics

// cs_validate_page runs on every page-in, so each patch is applied again whenever a bluetoothd page is evicted
// and read back. To make it visible what really got patched, every patch counts how often it was applied and at
// how many distinct places of the binary (file offsets). "sites" is the number to check: 1 for the a2dpcheck
// gate and every advsets patch; more means the pattern also hit unrelated code, 0 that it never matched (other
// macOS build, or the pattern straddles a page boundary). Published as the btlfx-patched property:
//   ioreg -r -c BlueToolFixup -d1 | grep btlfx-patched
// New sites are logged once; re-validations of a known site are only counted.

static constexpr size_t kMaxTrackedSites = 8;

struct PatchStat {
    const char *name;
    bool active;                                   // enabled for this boot (boot args, macOS version)
    volatile SInt32 applied;                       // replacements, including re-validated pages
    volatile SInt32 sites;                         // distinct file offsets, up to kMaxTrackedSites
    volatile UInt64 siteOffsets[kMaxTrackedSites]; // file offset + 1, 0 = free slot
    volatile bool moreSites;                       // more distinct sites than tracked
};

enum PatchId : size_t {
    kPatchSkipUpdate,
    kPatchBlueToolBoardId,
    kPatchVendorCheck,
    kPatchBadChipset,
    kPatchBadChipset13_3,
    kPatchBadChipset15_4,
    kPatchNvramCheck,
    kPatchBoardId,
    kPatchAddressCheck,
    kPatchA2DPGate,
    kPatchSpecAdvSetsCap,
    kPatchExtScanCap,
    kPatchLeEventMask,
    kPatchAdvSetAddr,
    kPatchExtCreateConn,
    kPatchAdvSetAddrResend,
    kPatchCount
};

static PatchStat patchStats[kPatchCount] = {
    { "BlueTool skip firmware update" },
    { "BlueTool board-id" },
    { "vendor check" },
    { "bad chipset check" },
    { "bad chipset check 13.3" },
    { "bad chipset check 15.4" },
    { "internal controller NVRAM check" },
    { "board-id" },
    { "address check" },
    { "a2dpcheck fast-connect gate" },
    { "advsets BT Spec advertising sets capability" },
    { "advsets extended scanning capability" },
    { "advsets LE event mask" },
    { "advsets advertising set random address" },
    { "advsets extended create connection" },
    { "advsets advertising set address resend" },
};

// Property updates allocate and take registry locks, which must not happen inside cs_validate_page (VM fault
// path). The page-in path only bumps counters and queues this thread call.
static thread_call_t patchStatsCall {nullptr};
static IOLock *statsLock;
static bool statsQueued;
static IOService *patchStatsService {nullptr};

static void updatePatchStats(thread_call_param_t, thread_call_param_t) {
    __atomic_store_n(&statsQueued, false, __ATOMIC_RELEASE);
    IOLockLock(statsLock);
    IOService *service = patchStatsService;
    if (service) service->retain();
    IOLockUnlock(statsLock);
    if (!service)
        return;
    OSDictionary *all = OSDictionary::withCapacity(kPatchCount);
    if (!all) {
        service->release();
        return;
    }
    for (size_t i = 0; i < kPatchCount; i++) {
        const PatchStat &stat = patchStats[i];
        if (!stat.active)
            continue;
        OSDictionary *entry = OSDictionary::withCapacity(3);
        OSNumber *applied = OSNumber::withNumber(__atomic_load_n(&stat.applied, __ATOMIC_RELAXED), 32);
        OSNumber *sites = OSNumber::withNumber(__atomic_load_n(&stat.sites, __ATOMIC_RELAXED), 32);
        if (entry && applied && sites) {
            entry->setObject("applied", applied);
            entry->setObject("sites", sites);
            if (__atomic_load_n(&stat.moreSites, __ATOMIC_RELAXED))
                entry->setObject("more-sites", kOSBooleanTrue);
            all->setObject(stat.name, entry);
        }
        OSSafeReleaseNULL(applied);
        OSSafeReleaseNULL(sites);
        OSSafeReleaseNULL(entry);
    }
    service->setProperty("btlfx-patched", all);
    all->release();
    service->release();
}

static void schedulePatchStatsUpdate() {
    // Coalesces: a call that is already pending is not queued twice
    if (patchStatsCall && !__atomic_exchange_n(&statsQueued, true, __ATOMIC_ACQ_REL)) {
        uint64_t deadline;
        clock_interval_to_deadline(250, kMillisecondScale, &deadline);
        thread_call_enter_delayed(patchStatsCall, deadline);
    }
}

// Runs once, in the page-in path: only flags and the coalesced stats call
static void refuseSpecAdvSets() {
    SYSLOG(MODULE_SHORT, "[advsets] tested on build 25E253 only, not applied on build %s (-btlfxadvsetsforce overrides)", kernelOsVersion);
    for (size_t i = kPatchSpecAdvSetsCap; i <= kPatchAdvSetAddrResend; i++)
        patchStats[i].active = false;
    schedulePatchStatsUpdate();
}

// Returns true when this offset was not seen before
static bool recordSite(PatchStat &stat, UInt64 fileOffset) {
    const UInt64 key = fileOffset + 1;
    for (size_t i = 0; i < kMaxTrackedSites; i++) {
        UInt64 current = __atomic_load_n(&stat.siteOffsets[i], __ATOMIC_ACQUIRE);
        if (current == key)
            return false;
        if (current == 0) {
            if (OSCompareAndSwap64(0, key, &stat.siteOffsets[i])) {
                OSIncrementAtomic(&stat.sites);
                return true;
            }
            // Another CPU took the slot first; it may have stored this very offset
            if (__atomic_load_n(&stat.siteOffsets[i], __ATOMIC_ACQUIRE) == key)
                return false;
        }
    }
    return !__atomic_exchange_n(&stat.moreSites, true, __ATOMIC_RELAXED);
}

// The a2dpcheck gate and the advsets edits hit exactly one place of bluetoothd. A second match in the same page means
// the pattern is not the code it was written for, so that page is left alone. The upstream patches (vendor and chipset
// checks, NVRAM, address check) keep patching every match, as BlueToolFixup always did.
static bool requiresUniqueMatch(PatchId id) {
    return id >= kPatchA2DPGate && id <= kPatchAdvSetAddrResend;
}

// Replaces every match of find in the page, one at a time so that each site can be recorded. Masks, when given,
// are findSize long; replaceSize may differ from findSize only without masks.
static size_t applyPatch(PatchId id, const void *data, memory_object_offset_t pageOffset, const char *path,
                         const void *find, size_t findSize, const void *findMask,
                         const void *replace, size_t replaceSize, const void *replaceMask) {
    PatchStat &stat = patchStats[id];
    auto page = static_cast<uint8_t *>(const_cast<void *>(data));
    size_t applied = 0;
    size_t offset = 0;
    if (!findSize || findSize > PAGE_SIZE || replaceSize > findSize)
        return 0;
    while (offset <= PAGE_SIZE - findSize) {
        size_t found = offset;
        if (!KernelPatcher::findPattern(find, findMask, findSize, page, PAGE_SIZE, &found))
            break;
        // Single-site edits must have an unambiguous match in this page.
        if (requiresUniqueMatch(id)) {
            size_t second = found + 1;
            if (KernelPatcher::findPattern(find, findMask, findSize, page, PAGE_SIZE, &second))
                return 0;
        }
        // Patch exactly this occurrence
        if (!KernelPatcher::findAndReplaceWithMask(page + found, findSize, find, findSize, findMask, findMask ? findSize : 0,
                                                   replace, replaceSize, replaceMask, replaceMask ? findSize : 0, 1, 0))
            break;
        applied++;
        const UInt64 fileOffset = static_cast<UInt64>(pageOffset) + found;
        if (recordSite(stat, fileOffset))
            SYSLOG(MODULE_SHORT, "patched %s in %s at 0x%llx", stat.name, path, fileOffset);
        offset = found + findSize;
    }
    if (applied) {
        OSAddAtomic(static_cast<SInt32>(applied), &stat.applied);
        schedulePatchStatsUpdate();
    }
    return applied;
}

template <size_t findSize, size_t replaceSize>
static inline size_t applyPatch(PatchId id, const void *data, memory_object_offset_t pageOffset, const char *path,
                                const uint8_t (&find)[findSize], const uint8_t (&replace)[replaceSize]) {
    return applyPatch(id, data, pageOffset, path, find, findSize, nullptr, replace, replaceSize, nullptr);
}

template <size_t size>
static inline size_t applyPatchWithMask(PatchId id, const void *data, memory_object_offset_t pageOffset, const char *path,
                                        const uint8_t (&find)[size], const uint8_t (&findMask)[size],
                                        const uint8_t (&replace)[size], const uint8_t (&replaceMask)[size]) {
    return applyPatch(id, data, pageOffset, path, find, size, findMask, replace, size, replaceMask);
}

bool BlueToolFixup::start(IOService *provider) {
    if (!IOService::start(provider)) {
        SYSLOG("init", "failed to start the parent");
        return false;
    }
    setProperty("VersionInfo", kextVersion);
    if (statsLock) {
        IOLockLock(statsLock);
        patchStatsService = this;
        IOLockUnlock(statsLock);
        schedulePatchStatsUpdate();
    }
    registerService();

    return true;
}


void BlueToolFixup::stop(IOService *provider) {
    if (statsLock) {
        IOLockLock(statsLock);
        if (patchStatsService == this) patchStatsService = nullptr;
        IOLockUnlock(statsLock);
    }
    IOService::stop(provider);
}

#pragma mark - Patched functions

static void patched_cs_validate_page(vnode_t vp, memory_object_t pager, memory_object_offset_t page_offset, const void *data, int *validated_p, int *tainted_p, int *nx_p) {
    char path[PATH_MAX];
    int pathlen = PATH_MAX;
    FunctionCast(patched_cs_validate_page, orig_cs_validate)(vp, pager, page_offset, data, validated_p, tainted_p, nx_p);
    if (!vp || !data || !validated_p || !*validated_p || !tainted_p || *tainted_p)
        return;
    static constexpr size_t dirLength = sizeof("/usr/sbin/")-1;
    if (vn_getpath(vp, path, &pathlen) == 0 && UNLIKELY(strncmp(path, "/usr/sbin/", dirLength) == 0)) {
        if (strcmp(path + dirLength, "BlueTool") == 0) {
            applyPatch(kPatchSkipUpdate, data, page_offset, path, kSkipUpdateFilePathOriginal, kSkipUpdateFilePathPatched);
            if (shouldPatchBoardId)
                applyPatch(kPatchBlueToolBoardId, data, page_offset, path, boardIdsWithUSBBluetooth[0], kBoardIdSize, nullptr,
                           replacementBoardId, sizeof(replacementBoardId), nullptr);
        }
        else if (strcmp(path + dirLength, "bluetoothd") == 0) {
            applyPatch(kPatchVendorCheck, data, page_offset, path, kVendorCheckOriginal, kVendorCheckPatched);
            applyPatch(kPatchBadChipset, data, page_offset, path, kBadChipsetCheckOriginal, kBadChipsetCheckPatched);
            if (patchStats[kPatchBadChipset13_3].active)
                applyPatch(kPatchBadChipset13_3, data, page_offset, path, kBadChipsetCheckOriginal13_3, kBadChipsetCheckPatched13_3);
            if (patchStats[kPatchBadChipset15_4].active)
                applyPatch(kPatchBadChipset15_4, data, page_offset, path, kBadChipsetCheckOriginal15_4, kBadChipsetCheckPatched15_4);
            if (shouldPatchNvramCheck)
                applyPatch(kPatchNvramCheck, data, page_offset, path, kSkipInternalControllerNVRAMCheck13_3, sizeof(kSkipInternalControllerNVRAMCheck13_3),
                           kSkipInternalControllerNVRAMCheckMask13_3, kSkipInternalControllerNVRAMCheckPatched13_3,
                           sizeof(kSkipInternalControllerNVRAMCheckPatched13_3), nullptr);
            if (shouldPatchBoardId)
                applyPatch(kPatchBoardId, data, page_offset, path, boardIdsWithUSBBluetooth[0], kBoardIdSize, nullptr,
                           replacementBoardId, sizeof(replacementBoardId), nullptr);
            if (shouldPatchAddress)
                applyPatchWithMask(kPatchAddressCheck, data, page_offset, path, kSkipAddressCheckOriginal, kSkipAddressCheckMask,
                                   kSkipAddressCheckPatched, kSkipAddressCheckMask);
            if (shouldPatchA2DPCheck) {
                // macOS 14.x/15.x/26.x (Sonoma/Sequoia/Tahoe) pattern
                if (getKernelVersion() >= KernelVersion::Sonoma)
                    applyPatchWithMask(kPatchA2DPGate, data, page_offset, path, kFastConnectA2DPGateOriginal14_0, kFastConnectA2DPGateMask14_0,
                                       kFastConnectA2DPGatePatched14_0, kFastConnectA2DPGateMask14_0);
                // macOS 12.x/13.x (Monterey/Ventura) pattern
                else
                    applyPatchWithMask(kPatchA2DPGate, data, page_offset, path, kFastConnectA2DPGateOriginal12_0, kFastConnectA2DPGateMask12_0,
                                       kFastConnectA2DPGatePatched12_0, kFastConnectA2DPGateMask12_0);
            }
            if (shouldPatchSpecAdvSets && specAdvSetsAllowed()) {
                applyPatch(kPatchSpecAdvSetsCap, data, page_offset, path, kSpecAdvSetsCapOriginal26, kSpecAdvSetsCapPatched26);
                applyPatch(kPatchExtScanCap, data, page_offset, path, kExtScanCapOriginal26, kSpecAdvSetsCapPatched26);
                applyPatch(kPatchLeEventMask, data, page_offset, path, kLeEventMaskOriginal26, kLeEventMaskPatched26);
                applyPatch(kPatchAdvSetAddr, data, page_offset, path, kAdvSetAddrOriginal26, kAdvSetAddrPatched26);
                applyPatch(kPatchExtCreateConn, data, page_offset, path, kExtCreateConnOriginal26, kExtCreateConnPatched26);
                applyPatch(kPatchAdvSetAddrResend, data, page_offset, path, kAdvSetAddrResendOriginal26, kAdvSetAddrResendPatched26);
            }
        }
    }
}


#pragma mark - Patches on start/stop

static void pluginStart() {
    SYSLOG(MODULE_SHORT, "start");
    // There is no point in routing cs_validate_range, because this kext should only be running on Monterey+
    if (getKernelVersion() < KernelVersion::Monterey)
        return;
    statsLock = IOLockAlloc();
    if (statsLock)
        patchStatsCall = thread_call_allocate_with_options(updatePatchStats, nullptr,
            THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    lilu.onPatcherLoadForce([](void *user, KernelPatcher &patcher) {
        const char *board = BaseDeviceInfo::get().boardIdentifier;
        size_t len = 0;
        while (board && len < kBoardIdSize && board[len])
            len++;
        if ((len + 1 == kBoardIdSize || len + 1 == kBoardIdSizeLegacy) &&
            !memcmp(board, "Mac-", 4)) {
            shouldPatchBoardId = getKernelVersion() < KernelVersion::Sonoma || checkKernelArgument("-btlfxboardid");
            for (size_t i = 4; i < len; ++i)
                if (!((board[i] >= '0' && board[i] <= '9') || (board[i] >= 'A' && board[i] <= 'F') ||
                      (board[i] >= 'a' && board[i] <= 'f')))
                    shouldPatchBoardId = false;
            for (const auto &known : boardIdsWithUSBBluetooth)
                if (!strcmp(board, known))
                    shouldPatchBoardId = false;
            if (shouldPatchBoardId)
                memcpy(replacementBoardId, board, len);
        }
        if ((getKernelVersion() == KernelVersion::Monterey && getKernelMinorVersion() >= 5) || getKernelVersion() > KernelVersion::Monterey)
            // 12.4 Beta 3+, XNU 21.5
            shouldPatchAddress = checkKernelArgument("-btlfxallowanyaddr");
        if (getKernelVersion() <= KernelVersion::Sonoma)
            shouldPatchNvramCheck = checkKernelArgument("-btlfxnvramcheck");
        shouldPatchA2DPCheck = getKernelVersion() <= KernelVersion::Tahoe && checkKernelArgument("-btlfxa2dpcheck");
        shouldPatchSpecAdvSets = checkKernelArgument("-btlfxadvsets");
        specAdvSetsForced = shouldPatchSpecAdvSets && checkKernelArgument("-btlfxadvsetsforce");
        if (shouldPatchSpecAdvSets && !specAdvSetsForced) {
            if (getKernelVersion() != KernelVersion::Tahoe) {
                SYSLOG(MODULE_SHORT, "[advsets] tested on build 25E253 only, not applied on Darwin %d.%d (-btlfxadvsetsforce overrides)",
                       getKernelVersion(), getKernelMinorVersion());
                shouldPatchSpecAdvSets = false;
            } else {
                kernelOsVersion = reinterpret_cast<const char *>(patcher.solveSymbol(KernelPatcher::KernelID, "_osversion"));
                if (!kernelOsVersion) {
                    patcher.clearError();
                    SYSLOG(MODULE_SHORT, "[advsets] cannot resolve osversion to check the build, not applied (-btlfxadvsetsforce overrides)");
                    shouldPatchSpecAdvSets = false;
                }
            }
        }
        if (shouldPatchSpecAdvSets)
            SYSLOG(MODULE_SHORT, "[advsets] BT Spec LE advertising sets for third-party controllers %s",
                   specAdvSetsForced ? "enabled (forced)" : "requested, applied once bluetoothd runs on build 25E253");
        if (shouldPatchA2DPCheck)
            SYSLOG(MODULE_SHORT, "[a2dpcheck] bluetoothd Read Clock workaround enabled (fast-connect A2DP bypass)");
        // Which patches this boot uses: only those are listed in btlfx-patched
        patchStats[kPatchSkipUpdate].active = true;
        patchStats[kPatchBlueToolBoardId].active = shouldPatchBoardId;
        patchStats[kPatchVendorCheck].active = true;
        patchStats[kPatchBadChipset].active = true;
        patchStats[kPatchBadChipset13_3].active = getKernelVersion() >= KernelVersion::Ventura && getKernelVersion() < KernelVersion::Sequoia;
        patchStats[kPatchBadChipset15_4].active = getKernelVersion() >= KernelVersion::Sequoia;
        patchStats[kPatchNvramCheck].active = shouldPatchNvramCheck;
        patchStats[kPatchBoardId].active = shouldPatchBoardId;
        patchStats[kPatchAddressCheck].active = shouldPatchAddress;
        patchStats[kPatchA2DPGate].active = shouldPatchA2DPCheck;
        for (size_t i = kPatchSpecAdvSetsCap; i <= kPatchAdvSetAddrResend; i++)
            patchStats[i].active = shouldPatchSpecAdvSets;
        KernelPatcher::RouteRequest csRoute = KernelPatcher::RouteRequest("_cs_validate_page", patched_cs_validate_page, orig_cs_validate);
        if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &csRoute, 1))
            SYSLOG(MODULE_SHORT, "failed to route cs validation pages");
    });
}

// Boot args.
static const char *bootargOff[] {
    "-btlfxoff"
};
static const char *bootargDebug[] {
    "-btlfxdbg"
};
static const char *bootargBeta[] {
    "-btlfxbeta"
};

// Plugin configuration.
PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Monterey,
    KernelVersion::Tahoe,
    pluginStart
};
