// ARMSX2Bridge.mm — ObjC bridge implementation
// SPDX-License-Identifier: GPL-3.0+

#import "ARMSX2Bridge.h"
#include "common/Darwin/DarwinMisc.h"
#include <SDL3/SDL.h>

extern "C" void ARMSX2_SetSDLFullscreen(bool enabled);
#include "Common.h"
#include "CDVD/CDVD.h"
#include "CDVD/CDVDcommon.h"
#include "VMManager.h"
#include "Patch.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "Counters.h"
#include "GS/GSState.h"
#include "GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ZipHelpers.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <future>
#include <optional>
#include <string_view>
#include <vector>

// Access the global settings interface from ios_main.mm
extern INISettingsInterface* g_p44_settings_interface;
extern "C" void ARMSX2_PrepareGameRenderViewForCurrentRenderer(const char* reason);

static NSDate* s_lastNVMSaveDate = nil;

@implementation ARMSX2SaveStateSlotInfo
@end

static int* ARMSX2JITBisectFlagPtr(NSString* key)
{
    if ([key isEqualToString:@"COP1EverythingOnly"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_ONLY;
    if ([key isEqualToString:@"COP1EverythingPlusLoadStore"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_LOADSTORE;
    if ([key isEqualToString:@"COP1EverythingPlusMMI"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_MMI;
    if ([key isEqualToString:@"COP1EverythingPlusCOP2VU"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_COP2_VU;
    if ([key isEqualToString:@"COP1EverythingPlusMultDiv"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_MULTDIV;
    if ([key isEqualToString:@"COP1EverythingPlusShifts"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_SHIFTS;
    if ([key isEqualToString:@"COP1EverythingPlusMoves"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_MOVES;
    if ([key isEqualToString:@"COP1EverythingPlusIntegerALU"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_INTEGER_ALU;
    if ([key isEqualToString:@"COP1EverythingPlusBranches"]) return &DarwinMisc::iPSX2_BISECT_COP1_EVERYTHING_PLUS_BRANCHES;
    return nullptr;
}

static void ARMSX2ApplyJITBisectFlag(NSString* key, BOOL enabled)
{
    if (int* flag = ARMSX2JITBisectFlagPtr(key))
        *flag = enabled ? 1 : 0;
}

static NSString* ARMSX2NSStringFromStdString(const std::string& value)
{
    if (value.empty())
        return @"";

    NSString* string = [NSString stringWithUTF8String:value.c_str()];
    return string ?: @"";
}

static NSString* ARMSX2NSStringFromStringView(std::string_view value)
{
    if (value.empty())
        return @"";

    NSString* string = [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
    return string ?: @"";
}

static BOOL ARMSX2GetCurrentSaveStateIdentity(std::string* serial, u32* crc)
{
    if (!VMManager::HasValidVM())
        return NO;

    const std::string currentSerial = VMManager::GetDiscSerial();
    const u32 currentCRC = VMManager::GetDiscCRC();
    if (currentSerial.empty() || currentCRC == 0)
        return NO;

    if (serial)
        *serial = currentSerial;
    if (crc)
        *crc = currentCRC;
    return YES;
}

static NSString* ARMSX2ResolveISOPath(NSString* isoName)
{
    if (isoName.length == 0)
        return nil;

    NSFileManager* fm = [NSFileManager defaultManager];
    NSString* isoPath = [[ARMSX2Bridge isoDirectory] stringByAppendingPathComponent:isoName];
    if ([fm fileExistsAtPath:isoPath])
        return isoPath;

    NSString* docsPath = [[ARMSX2Bridge documentsDirectory] stringByAppendingPathComponent:isoName];
    if ([fm fileExistsAtPath:docsPath])
        return docsPath;

    return nil;
}

static NSString* ARMSX2SanitizedMemoryCardName(NSString* name)
{
    NSString* trimmed = [name stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (trimmed.length == 0)
        return @"";

    NSMutableString* sanitized = [NSMutableString stringWithCapacity:trimmed.length];
    NSCharacterSet* invalid = [NSCharacterSet characterSetWithCharactersInString:@"/\\:?%*|\"<>"];
    for (NSUInteger i = 0; i < trimmed.length; i++) {
        unichar ch = [trimmed characterAtIndex:i];
        [sanitized appendString:[invalid characterIsMember:ch] ? @"_" : [NSString stringWithCharacters:&ch length:1]];
    }

    while ([sanitized containsString:@".."])
        [sanitized replaceOccurrencesOfString:@".." withString:@"_" options:0 range:NSMakeRange(0, sanitized.length)];

    if (sanitized.pathExtension.length == 0)
        [sanitized appendString:@".ps2"];

    return sanitized;
}

static MemoryCardFileType ARMSX2MemoryCardFileTypeForSizeMB(NSInteger sizeMB)
{
    switch (sizeMB) {
    case 8:
        return MemoryCardFileType::PS2_8MB;
    case 16:
        return MemoryCardFileType::PS2_16MB;
    case 32:
        return MemoryCardFileType::PS2_32MB;
    case 64:
        return MemoryCardFileType::PS2_64MB;
    default:
        return MemoryCardFileType::Unknown;
    }
}

static NSData* ARMSX2ReadSaveStatePreviewPNG(const std::string& path)
{
    if (path.empty())
        return nil;

    zip_error_t ze = {};
    auto zf = zip_open_managed(path.c_str(), ZIP_RDONLY, &ze);
    if (!zf)
        return nil;

    auto zff = zip_fopen_managed(zf.get(), "Screenshot.png", 0);
    if (!zff)
        return nil;

    std::optional<std::vector<u8>> data = ReadBinaryFileInZip(zff.get());
    if (!data.has_value() || data->empty())
        return nil;

    return [NSData dataWithBytes:data->data() length:data->size()];
}

static void ARMSX2ApplyLiveGSBoolSetting(const char* section, const char* key, bool value)
{
    if (std::strcmp(section, "EmuCore/GS") != 0)
        return;

#define APPLY_OSD_BOOL(name) \
    do { \
        if (std::strcmp(key, #name) == 0) { \
            EmuConfig.GS.name = value; \
            GSConfig.name = value; \
            return; \
        } \
    } while (0)

    APPLY_OSD_BOOL(OsdShowFPS);
    APPLY_OSD_BOOL(OsdShowVPS);
    APPLY_OSD_BOOL(OsdShowSpeed);
    APPLY_OSD_BOOL(OsdShowCPU);
    APPLY_OSD_BOOL(OsdShowGPU);
    APPLY_OSD_BOOL(OsdShowResolution);
    APPLY_OSD_BOOL(OsdShowGSStats);
    APPLY_OSD_BOOL(OsdShowIndicators);
    APPLY_OSD_BOOL(OsdShowSettings);
    APPLY_OSD_BOOL(OsdShowInputs);
    APPLY_OSD_BOOL(OsdShowFrameTimes);
    APPLY_OSD_BOOL(OsdShowVersion);
    APPLY_OSD_BOOL(OsdShowHardwareInfo);
    APPLY_OSD_BOOL(OsdShowVideoCapture);
    APPLY_OSD_BOOL(OsdShowInputRec);

#undef APPLY_OSD_BOOL
}

static void ARMSX2ApplyLiveGSIntSetting(const char* section, const char* key, int value)
{
    if (std::strcmp(section, "EmuCore/GS") != 0 || std::strcmp(key, "OsdPerformancePos") != 0)
        return;

    const int clamped = std::clamp(value, static_cast<int>(OsdOverlayPos::None), static_cast<int>(OsdOverlayPos::TopRight));
    EmuConfig.GS.OsdPerformancePos = static_cast<OsdOverlayPos>(clamped);
    GSConfig.OsdPerformancePos = static_cast<OsdOverlayPos>(clamped);
}

static void ARMSX2ApplyLiveFloatSetting(const char* section, const char* key, float value)
{
    if (std::strcmp(section, "Framerate") == 0) {
        const float clamped = std::clamp(value, 0.05f, 10.0f);
        if (std::strcmp(key, "NominalScalar") == 0)
            EmuConfig.EmulationSpeed.NominalScalar = clamped;
        else if (std::strcmp(key, "TurboScalar") == 0)
            EmuConfig.EmulationSpeed.TurboScalar = clamped;
        else if (std::strcmp(key, "SlomoScalar") == 0)
            EmuConfig.EmulationSpeed.SlomoScalar = clamped;
        else
            return;

        VMManager::UpdateTargetSpeed();
        NSLog(@"[ARMSX2Bridge] live framerate %s/%s=%0.3f", section, key, clamped);
        return;
    }

    if (std::strcmp(section, "EmuCore/GS") != 0)
        return;

    if (std::strcmp(key, "FramerateNTSC") == 0) {
        EmuConfig.GS.FramerateNTSC = value;
        VMManager::UpdateTargetSpeed();
        return;
    }
    if (std::strcmp(key, "FrameratePAL") == 0) {
        EmuConfig.GS.FrameratePAL = value;
        VMManager::UpdateTargetSpeed();
        return;
    }
    if (std::strcmp(key, "upscale_multiplier") == 0) {
        const float clamped = std::clamp(value, 0.25f, 8.0f);
        EmuConfig.GS.UpscaleMultiplier = clamped;
        GSConfig.UpscaleMultiplier = clamped;
        return;
    }
}

@implementation ARMSX2Bridge

+ (UIView *)gameRenderView {
    extern UIView* g_gameRenderView;
    return g_gameRenderView;
}

+ (void)prepareGameRenderViewForCurrentRenderer {
    ARMSX2_PrepareGameRenderViewForCurrentRenderer("swift_preboot");
}

+ (void)saveNVRAM {
    cdvdSaveNVRAM();
    s_lastNVMSaveDate = [NSDate date];
    NSLog(@"[ARMSX2Bridge] NVM saved at %@", s_lastNVMSaveDate);
}

+ (void)saveMemoryCards {
    // FileMcd_EmuClose triggers save on all open memory cards
    // For now, MC saves happen automatically via the existing PCSX2 MC system
    NSLog(@"[ARMSX2Bridge] Memory card save requested");
}

+ (void)saveAllState {
    [self saveNVRAM];
    [self saveMemoryCards];
}

+ (BOOL)isRunning {
    return VMManager::GetState() == VMState::Running;
}

+ (nullable NSDate *)lastNVMSaveDate {
    return s_lastNVMSaveDate;
}

+ (nullable NSString *)nvmFilePath {
    // NVM path is BIOS path with .nvm extension
    // We can't easily access BiosPath from here, so return nil for now
    return nil;
}

+ (BOOL)nvmFileExists {
    NSString* path = [self nvmFilePath];
    if (!path) return NO;
    return [[NSFileManager defaultManager] fileExistsAtPath:path];
}

+ (void)setPadButton:(ARMSX2PadButton)button pressed:(BOOL)pressed {
    auto* pad = static_cast<PadDualshock2*>(Pad::GetPad(0, 0));
    if (!pad) return;

    static const u32 buttonMap[] = {
        PadDualshock2::Inputs::PAD_UP,       // Up
        PadDualshock2::Inputs::PAD_DOWN,     // Down
        PadDualshock2::Inputs::PAD_LEFT,     // Left
        PadDualshock2::Inputs::PAD_RIGHT,    // Right
        PadDualshock2::Inputs::PAD_CROSS,    // Cross
        PadDualshock2::Inputs::PAD_CIRCLE,   // Circle
        PadDualshock2::Inputs::PAD_SQUARE,   // Square
        PadDualshock2::Inputs::PAD_TRIANGLE, // Triangle
        PadDualshock2::Inputs::PAD_L1,       // L1
        PadDualshock2::Inputs::PAD_R1,       // R1
        PadDualshock2::Inputs::PAD_L2,       // L2
        PadDualshock2::Inputs::PAD_R2,       // R2
        PadDualshock2::Inputs::PAD_START,    // Start
        PadDualshock2::Inputs::PAD_SELECT,   // Select
        PadDualshock2::Inputs::PAD_L3,       // L3
        PadDualshock2::Inputs::PAD_R3,       // R3
    };

    if ((int)button < (int)(sizeof(buttonMap)/sizeof(buttonMap[0]))) {
        u32 idx = buttonMap[(int)button];
        pad->Set(idx, pressed ? 1.0f : 0.0f);
        // Update touch state so PumpMessagesOnCPUThread doesn't override
        extern bool g_touchPadState[64];
        if (idx < 64) g_touchPadState[idx] = pressed;
    }
}

+ (void)setLeftStickX:(float)x Y:(float)y {
    auto* pad = static_cast<PadDualshock2*>(Pad::GetPad(0, 0));
    if (!pad) return;
    // Convert axis (-1..+1) to individual direction values (0..1)
    pad->Set(PadDualshock2::Inputs::PAD_L_RIGHT, x > 0 ? x : 0.0f);
    pad->Set(PadDualshock2::Inputs::PAD_L_LEFT, x < 0 ? -x : 0.0f);
    pad->Set(PadDualshock2::Inputs::PAD_L_DOWN, y > 0 ? y : 0.0f);
    pad->Set(PadDualshock2::Inputs::PAD_L_UP, y < 0 ? -y : 0.0f);
}

+ (void)setRightStickX:(float)x Y:(float)y {
    auto* pad = static_cast<PadDualshock2*>(Pad::GetPad(0, 0));
    if (!pad) return;
    pad->Set(PadDualshock2::Inputs::PAD_R_RIGHT, x > 0 ? x : 0.0f);
    pad->Set(PadDualshock2::Inputs::PAD_R_LEFT, x < 0 ? -x : 0.0f);
    pad->Set(PadDualshock2::Inputs::PAD_R_DOWN, y > 0 ? y : 0.0f);
    pad->Set(PadDualshock2::Inputs::PAD_R_UP, y < 0 ? -y : 0.0f);
}

+ (nonnull NSString *)biosName {
    return @"PS2";
}

+ (void)requestVMStop {
    extern std::atomic<bool> s_requestVMStop;
    s_requestVMStop.store(true);
    NSLog(@"[ARMSX2Bridge] VM stop requested");
}

+ (void)setFullScreen:(BOOL)enabled {
    ARMSX2_SetSDLFullscreen(enabled ? true : false);
}

+ (nonnull NSString *)buildVersion {
    NSString *ver = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"?";
    return [NSString stringWithFormat:@"ARMSX2 iOS v%@", ver];
}

+ (nullable NSString *)currentISOPath {
    NSString *docsPath = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    NSString *iniPath = [docsPath stringByAppendingPathComponent:@"ARMSX2-iOS.ini"];
    if (![[NSFileManager defaultManager] fileExistsAtPath:iniPath])
        iniPath = [docsPath stringByAppendingPathComponent:@"PCSX2-iOS.ini"];
    // Read BootISO from INI
    FILE *f = fopen(iniPath.UTF8String, "r");
    if (!f) return nil;
    char line[512];
    bool inSection = false;
    NSString *result = nil;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "[GameISO]")) { inSection = true; continue; }
        if (line[0] == '[') { inSection = false; continue; }
        if (inSection && strstr(line, "BootISO")) {
            char *eq = strchr(line, '=');
            if (eq) {
                eq++;
                while (*eq == ' ') eq++;
                // Remove trailing newline
                char *nl = strchr(eq, '\n'); if (nl) *nl = 0;
                char *cr = strchr(eq, '\r'); if (cr) *cr = 0;
                if (strlen(eq) > 0) result = [NSString stringWithUTF8String:eq];
            }
        }
    }
    fclose(f);
    return result;
}

+ (nonnull NSString *)isoDirectory {
    NSString *docsPath = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    NSString *isoDir = [docsPath stringByAppendingPathComponent:@"iso"];
    [[NSFileManager defaultManager] createDirectoryAtPath:isoDir withIntermediateDirectories:YES attributes:nil error:nil];
    return isoDir;
}

+ (nonnull NSString *)documentsDirectory {
    return [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
}

+ (nonnull NSArray<NSString *> *)availableISOs {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableSet *seen = [NSMutableSet set];
    NSMutableArray *isos = [NSMutableArray array];

    // Helper block: scan a directory for ISO files
    void (^scanDir)(NSString *) = ^(NSString *dir) {
        NSArray *files = [fm contentsOfDirectoryAtPath:dir error:nil];
        for (NSString *file in files) {
            if ([seen containsObject:file]) continue;
            NSString *ext = file.pathExtension.lowercaseString;
            if ([ext isEqualToString:@"iso"] || [ext isEqualToString:@"img"] || [ext isEqualToString:@"chd"] ||
                [ext isEqualToString:@"cso"] || [ext isEqualToString:@"zso"] || [ext isEqualToString:@"gz"] ||
                [ext isEqualToString:@"elf"]) {
                [isos addObject:file];
                [seen addObject:file];
            } else if ([ext isEqualToString:@"bin"]) {
// .bin > 50MB treated as game image
                NSString *fullPath = [dir stringByAppendingPathComponent:file];
                NSDictionary *attrs = [fm attributesOfItemAtPath:fullPath error:nil];
                if ([attrs fileSize] > 50 * 1024 * 1024) {
                    [isos addObject:file];
                    [seen addObject:file];
                }
            }
        }
    };

    // Scan Documents/iso/ first, then Documents/ root
    scanDir([self isoDirectory]);
    NSString *docsPath = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    scanDir(docsPath);

    return isos;
}

+ (nonnull NSDictionary<NSString *, NSString *> *)gameMetadataForISO:(nonnull NSString *)isoName {
    if (isoName.length == 0)
        return @{};

    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *path = [[self isoDirectory] stringByAppendingPathComponent:isoName];
    if (![fm fileExistsAtPath:path]) {
        path = [[self documentsDirectory] stringByAppendingPathComponent:isoName];
    }

    NSMutableDictionary<NSString *, NSString *> *metadata = [NSMutableDictionary dictionary];
    metadata[@"fileTitle"] = isoName.stringByDeletingPathExtension ?: isoName;

    if (![fm fileExistsAtPath:path]) {
        return metadata;
    }

    GameList::Entry entry;
    if (GameList::PopulateEntryFromPath(path.UTF8String, &entry)) {
        NSString *title = ARMSX2NSStringFromStdString(entry.GetTitle(false));
        NSString *serial = ARMSX2NSStringFromStdString(entry.serial);
        const char *regionText = GameList::RegionToString(entry.region, false);

        if (title.length > 0)
            metadata[@"title"] = title;
        if (serial.length > 0)
            metadata[@"serial"] = serial;
        if (regionText && *regionText)
            metadata[@"region"] = @(regionText);

        NSLog(@"[ARMSX2 iOS Covers] metadata %@ title=%@ serial=%@ region=%@",
              isoName, metadata[@"title"] ?: @"", metadata[@"serial"] ?: @"", metadata[@"region"] ?: @"");
    } else {
        NSLog(@"[ARMSX2 iOS Covers] metadata unavailable %@", isoName);
    }

    return metadata;
}

+ (void)changeDiscToISO:(nonnull NSString *)isoName completion:(nullable ARMSX2SaveStateCompletion)completion {
    ARMSX2SaveStateCompletion callback = [completion copy];
    NSString* isoPath = ARMSX2ResolveISOPath(isoName);
    if (!isoPath || !VMManager::HasValidVM()) {
        NSLog(@"[ARMSX2Bridge] ChangeDisc rejected iso=%@ path=%@ validVM=%d", isoName, isoPath ?: @"", VMManager::HasValidVM() ? 1 : 0);
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(NO); });
        return;
    }

    const std::string nativePath(isoPath.UTF8String ?: "");
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        bool result = false;
        Host::RunOnCPUThread([nativePath, &result]() {
            result = VMManager::ChangeDisc(CDVD_SourceType::Iso, nativePath);
        }, true);

        NSLog(@"[ARMSX2Bridge] ChangeDisc iso=%@ result=%d", isoName, result ? 1 : 0);
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(result ? YES : NO); });
    });
}

+ (void)ejectDiscWithCompletion:(nullable ARMSX2SaveStateCompletion)completion {
    ARMSX2SaveStateCompletion callback = [completion copy];
    if (!VMManager::HasValidVM()) {
        NSLog(@"[ARMSX2Bridge] EjectDisc rejected validVM=0");
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(NO); });
        return;
    }

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        bool result = false;
        Host::RunOnCPUThread([&result]() {
            result = VMManager::ChangeDisc(CDVD_SourceType::NoDisc, {});
        }, true);

        NSLog(@"[ARMSX2Bridge] EjectDisc result=%d", result ? 1 : 0);
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(result ? YES : NO); });
    });
}

// Toggle overlay visibility via position (None vs TopRight).
// Individual OSD flags are controlled by preset in SettingsStore, not here.
+ (void)setPerformanceOverlayVisible:(BOOL)visible {
    if (visible) {
        GSConfig.OsdPerformancePos = EmuConfig.GS.OsdPerformancePos;
        // If user had None in config, default to TopRight
        if (GSConfig.OsdPerformancePos == OsdOverlayPos::None)
            GSConfig.OsdPerformancePos = OsdOverlayPos::TopRight;
    } else {
        GSConfig.OsdPerformancePos = OsdOverlayPos::None;
    }
}

+ (BOOL)isPerformanceOverlayVisible {
    return GSConfig.OsdPerformancePos != OsdOverlayPos::None;
}

// Apply OSD preset — sets ALL GSConfig flags to match the preset
+ (void)applyOsdPreset:(int)preset {
    // Clear everything first
    GSConfig.OsdShowFPS = false;
    GSConfig.OsdShowSpeed = false;
    GSConfig.OsdShowVPS = false;
    GSConfig.OsdShowCPU = false;
    GSConfig.OsdShowGPU = false;
    GSConfig.OsdShowResolution = false;
    GSConfig.OsdShowGSStats = false;
    GSConfig.OsdShowFrameTimes = false;
    GSConfig.OsdShowVersion = false;
    GSConfig.OsdShowHardwareInfo = false;
    GSConfig.OsdShowIndicators = false;
    GSConfig.OsdShowSettings = false;
    GSConfig.OsdShowInputs = false;
    GSConfig.OsdShowVideoCapture = false;
    GSConfig.OsdShowInputRec = false;

    switch (preset) {
    case 1: // simple: Android-style quick readout
        GSConfig.OsdShowFPS = true;
        GSConfig.OsdShowSpeed = true;
        GSConfig.OsdShowCPU = true;
        GSConfig.OsdShowIndicators = true;
        break;
    case 2: // detail: performance and renderer diagnostics
        GSConfig.OsdShowFPS = true;
        GSConfig.OsdShowVPS = true;
        GSConfig.OsdShowSpeed = true;
        GSConfig.OsdShowCPU = true;
        GSConfig.OsdShowGPU = true;
        GSConfig.OsdShowResolution = true;
        GSConfig.OsdShowIndicators = true;
        break;
    case 3: // full: closest to Android's full stats section
        GSConfig.OsdShowFPS = true;
        GSConfig.OsdShowVPS = true;
        GSConfig.OsdShowSpeed = true;
        GSConfig.OsdShowCPU = true;
        GSConfig.OsdShowGPU = true;
        GSConfig.OsdShowResolution = true;
        GSConfig.OsdShowGSStats = true;
        GSConfig.OsdShowFrameTimes = true;
        GSConfig.OsdShowVersion = true;
        GSConfig.OsdShowHardwareInfo = true;
        GSConfig.OsdShowIndicators = true;
        GSConfig.OsdShowSettings = true;
        GSConfig.OsdShowInputs = true;
        break;
    default: // 0 = off
        break;
    }

    EmuConfig.GS.OsdShowFPS = GSConfig.OsdShowFPS;
    EmuConfig.GS.OsdShowVPS = GSConfig.OsdShowVPS;
    EmuConfig.GS.OsdShowSpeed = GSConfig.OsdShowSpeed;
    EmuConfig.GS.OsdShowCPU = GSConfig.OsdShowCPU;
    EmuConfig.GS.OsdShowGPU = GSConfig.OsdShowGPU;
    EmuConfig.GS.OsdShowResolution = GSConfig.OsdShowResolution;
    EmuConfig.GS.OsdShowGSStats = GSConfig.OsdShowGSStats;
    EmuConfig.GS.OsdShowFrameTimes = GSConfig.OsdShowFrameTimes;
    EmuConfig.GS.OsdShowVersion = GSConfig.OsdShowVersion;
    EmuConfig.GS.OsdShowHardwareInfo = GSConfig.OsdShowHardwareInfo;
    EmuConfig.GS.OsdShowIndicators = GSConfig.OsdShowIndicators;
    EmuConfig.GS.OsdShowSettings = GSConfig.OsdShowSettings;
    EmuConfig.GS.OsdShowInputs = GSConfig.OsdShowInputs;
}

// ============================================================
// ISO / BIOS / Settings management
// ============================================================

#pragma mark - ISO boot

+ (void)bootISO:(nonnull NSString *)isoName {
    if (!g_p44_settings_interface) return;
    g_p44_settings_interface->SetStringValue("GameISO", "BootISO", isoName.UTF8String);
    g_p44_settings_interface->Save();
    NSLog(@"bootISO: set BootISO=%@", isoName);
}

#pragma mark - BIOS management

+ (nonnull NSString *)biosDirectory {
    NSString *docsPath = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    NSString *biosDir = [docsPath stringByAppendingPathComponent:@"bios"];
    [[NSFileManager defaultManager] createDirectoryAtPath:biosDir withIntermediateDirectories:YES attributes:nil error:nil];
    return biosDir;
}

+ (nonnull NSArray<NSString *> *)availableBIOSes {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableSet *seen = [NSMutableSet set];
    NSMutableArray *bioses = [NSMutableArray array];

    // Helper block: scan directory for BIOS files (>= 1MB .bin/.rom)
    void (^scanDir)(NSString *) = ^(NSString *dir) {
        NSArray *files = [fm contentsOfDirectoryAtPath:dir error:nil];
        for (NSString *file in files) {
            if ([seen containsObject:file]) continue;
            NSString *ext = file.pathExtension.lowercaseString;
            if ([ext isEqualToString:@"bin"] || [ext isEqualToString:@"rom"]) {
                NSString *fullPath = [dir stringByAppendingPathComponent:file];
                NSDictionary *attrs = [fm attributesOfItemAtPath:fullPath error:nil];
// BIOS files are >= 1MB and <= 50MB
                unsigned long long sz = [attrs fileSize];
                if (sz >= 1024 * 1024 && sz <= 50 * 1024 * 1024) {
                    [bioses addObject:file];
                    [seen addObject:file];
                }
            }
        }
    };

    scanDir([self biosDirectory]);
    return bioses;
}

+ (nonnull NSString *)defaultBIOSName {
    if (!g_p44_settings_interface) return @"";
    std::string val = g_p44_settings_interface->GetStringValue("Filenames", "BIOS", "");
    return [NSString stringWithUTF8String:val.c_str()];
}

+ (void)setDefaultBIOS:(nonnull NSString *)biosName {
    if (!g_p44_settings_interface) return;
    g_p44_settings_interface->SetStringValue("Filenames", "BIOS", biosName.UTF8String);
    g_p44_settings_interface->Save();
    EmuConfig.BaseFilenames.Bios = biosName.UTF8String;
    NSLog(@"setDefaultBIOS: %@", biosName);
}

#pragma mark - Favorites

+ (BOOL)isFavorite:(nonnull NSString *)isoName {
    if (!g_p44_settings_interface) return NO;
    return g_p44_settings_interface->GetBoolValue("Favorites", isoName.UTF8String, false);
}

+ (void)setFavorite:(nonnull NSString *)isoName favorite:(BOOL)favorite {
    if (!g_p44_settings_interface) return;
    g_p44_settings_interface->SetBoolValue("Favorites", isoName.UTF8String, favorite);
    g_p44_settings_interface->Save();
}

#pragma mark - INI generic getter/setter

+ (int)getINIInt:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(int)def {
    if (!g_p44_settings_interface) return def;
    return g_p44_settings_interface->GetIntValue(section.UTF8String, key.UTF8String, def);
}

+ (BOOL)getINIBool:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def {
    if (!g_p44_settings_interface) return def;
    return g_p44_settings_interface->GetBoolValue(section.UTF8String, key.UTF8String, def);
}

+ (float)getINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(float)def {
    if (!g_p44_settings_interface) return def;
    return g_p44_settings_interface->GetFloatValue(section.UTF8String, key.UTF8String, def);
}

+ (nonnull NSString *)getINIString:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(nonnull NSString *)def {
    if (!g_p44_settings_interface) return def;
    std::string val = g_p44_settings_interface->GetStringValue(section.UTF8String, key.UTF8String, def.UTF8String);
    return [NSString stringWithUTF8String:val.c_str()];
}

+ (void)setINIInt:(nonnull NSString *)section key:(nonnull NSString *)key value:(int)value {
    if (!g_p44_settings_interface) return;
    g_p44_settings_interface->SetIntValue(section.UTF8String, key.UTF8String, value);
    g_p44_settings_interface->Save();
    ARMSX2ApplyLiveGSIntSetting(section.UTF8String, key.UTF8String, value);
}

+ (void)setINIBool:(nonnull NSString *)section key:(nonnull NSString *)key value:(BOOL)value {
    if (!g_p44_settings_interface) return;
    g_p44_settings_interface->SetBoolValue(section.UTF8String, key.UTF8String, value);
    g_p44_settings_interface->Save();
    ARMSX2ApplyLiveGSBoolSetting(section.UTF8String, key.UTF8String, value);
}

+ (void)setINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key value:(float)value {
    if (!g_p44_settings_interface) return;
    g_p44_settings_interface->SetFloatValue(section.UTF8String, key.UTF8String, value);
    g_p44_settings_interface->Save();
    ARMSX2ApplyLiveFloatSetting(section.UTF8String, key.UTF8String, value);
}

+ (void)setINIString:(nonnull NSString *)section key:(nonnull NSString *)key value:(nonnull NSString *)value {
    if (!g_p44_settings_interface) return;
    g_p44_settings_interface->SetStringValue(section.UTF8String, key.UTF8String, value.UTF8String);
    g_p44_settings_interface->Save();
}

#pragma mark - Compatibility Lab

+ (BOOL)getJITBisectFlag:(nonnull NSString *)key defaultValue:(BOOL)def
{
    BOOL value = def;
    if (g_p44_settings_interface)
        value = g_p44_settings_interface->GetBoolValue("ARMSX2/JITBisect", key.UTF8String, def);

    ARMSX2ApplyJITBisectFlag(key, value);
    return value;
}

+ (void)setJITBisectFlag:(nonnull NSString *)key value:(BOOL)value
{
    ARMSX2ApplyJITBisectFlag(key, value);
    if (g_p44_settings_interface) {
        g_p44_settings_interface->SetBoolValue("ARMSX2/JITBisect", key.UTF8String, value);
        g_p44_settings_interface->Save();
    }
    NSLog(@"[ARMSX2Bridge] Compatibility Lab %@ %@", key, value ? @"ON" : @"OFF");
}

#pragma mark - VM lifecycle

+ (BOOL)isVMRunning {
    VMState st = VMManager::GetState();
    return st == VMState::Running || st == VMState::Paused;
}

+ (BOOL)hasBIOS {
    if (EmuConfig.BaseFilenames.Bios.empty()) return NO;
    std::string fullPath = Path::Combine(EmuFolders::Bios, EmuConfig.BaseFilenames.Bios);
    return FileSystem::FileExists(fullPath.c_str());
}

+ (void)requestVMBoot {
    [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2iOSRequestVMBoot" object:nil];
}

+ (void)requestVMShutdown {
    [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2iOSRequestVMShutdown" object:nil];
}

#pragma mark - Save states

+ (BOOL)hasValidSaveStateGame {
    return ARMSX2GetCurrentSaveStateIdentity(nullptr, nullptr);
}

+ (nonnull NSArray<ARMSX2SaveStateSlotInfo *> *)saveStateSlots {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2GetCurrentSaveStateIdentity(&serial, &crc))
        return @[];

    NSMutableArray<ARMSX2SaveStateSlotInfo *> *slots = [NSMutableArray arrayWithCapacity:VMManager::NUM_SAVE_STATE_SLOTS];
    NSFileManager *fm = [NSFileManager defaultManager];

    for (s32 slot = 1; slot <= VMManager::NUM_SAVE_STATE_SLOTS; slot++) {
        const std::string path = VMManager::GetSaveStateFileName(serial.c_str(), crc, slot);
        const BOOL occupied = !path.empty() && FileSystem::FileExists(path.c_str());
        NSString *nsPath = ARMSX2NSStringFromStdString(path);

        ARMSX2SaveStateSlotInfo *info = [ARMSX2SaveStateSlotInfo new];
        info.slot = slot;
        info.occupied = occupied;
        info.filePath = nsPath;
        info.fileName = ARMSX2NSStringFromStringView(Path::GetFileName(path));

        if (occupied) {
            NSDictionary<NSFileAttributeKey, id> *attrs = [fm attributesOfItemAtPath:nsPath error:nil];
            info.modifiedDate = attrs[NSFileModificationDate];
            info.previewPNGData = ARMSX2ReadSaveStatePreviewPNG(path);
        }

        [slots addObject:info];
    }

    return slots;
}

+ (void)saveStateToSlot:(NSInteger)slot completion:(nullable ARMSX2SaveStateCompletion)completion {
    const s32 nativeSlot = static_cast<s32>(slot);
    ARMSX2SaveStateCompletion callback = [completion copy];
    std::string serial;
    u32 crc = 0;
    if (nativeSlot < 1 || nativeSlot > VMManager::NUM_SAVE_STATE_SLOTS || !ARMSX2GetCurrentSaveStateIdentity(&serial, &crc)) {
        NSLog(@"[ARMSX2 iOS SaveState] save rejected slot=%d validGame=0", nativeSlot);
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(NO); });
        return;
    }

    const std::string targetPath = VMManager::GetSaveStateFileName(serial.c_str(), crc, nativeSlot);
    NSLog(@"[ARMSX2 iOS SaveState] save requested slot=%d path=%@", nativeSlot, ARMSX2NSStringFromStdString(targetPath));

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        bool result = false;
        Host::RunOnCPUThread([nativeSlot, &result]() {
            NSLog(@"[ARMSX2 iOS SaveState] CPU save start slot=%d", nativeSlot);
            result = VMManager::SaveStateToSlot(nativeSlot, true);
            NSLog(@"[ARMSX2 iOS SaveState] CPU save queued slot=%d result=%d", nativeSlot, result ? 1 : 0);
        }, true);

        if (result) {
            VMManager::WaitForSaveStateFlush();
            result = targetPath.empty() ? result : FileSystem::FileExists(targetPath.c_str());
        }

        NSLog(@"[ARMSX2 iOS SaveState] save finished slot=%d result=%d exists=%d",
              nativeSlot, result ? 1 : 0, (!targetPath.empty() && FileSystem::FileExists(targetPath.c_str())) ? 1 : 0);

        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(result ? YES : NO); });
    });
}

+ (void)loadStateFromSlot:(NSInteger)slot completion:(nullable ARMSX2SaveStateCompletion)completion {
    const s32 nativeSlot = static_cast<s32>(slot);
    ARMSX2SaveStateCompletion callback = [completion copy];
    std::string serial;
    u32 crc = 0;
    if (nativeSlot < 1 || nativeSlot > VMManager::NUM_SAVE_STATE_SLOTS || !ARMSX2GetCurrentSaveStateIdentity(&serial, &crc)) {
        NSLog(@"[ARMSX2 iOS SaveState] load rejected slot=%d validGame=0", nativeSlot);
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(NO); });
        return;
    }

    const std::string targetPath = VMManager::GetSaveStateFileName(serial.c_str(), crc, nativeSlot);
    NSLog(@"[ARMSX2 iOS SaveState] load requested slot=%d path=%@ exists=%d",
          nativeSlot, ARMSX2NSStringFromStdString(targetPath), (!targetPath.empty() && FileSystem::FileExists(targetPath.c_str())) ? 1 : 0);

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        bool result = false;
        Host::RunOnCPUThread([nativeSlot, &result]() {
            NSLog(@"[ARMSX2 iOS SaveState] CPU load start slot=%d", nativeSlot);
            result = VMManager::LoadStateFromSlot(nativeSlot);
            NSLog(@"[ARMSX2 iOS SaveState] CPU load finished slot=%d result=%d", nativeSlot, result ? 1 : 0);
        }, true);

        NSLog(@"[ARMSX2 iOS SaveState] load callback slot=%d result=%d", nativeSlot, result ? 1 : 0);

        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(result ? YES : NO); });
    });
}

#pragma mark - PNACH cheats/patches

+ (nullable NSString *)pnachPathForCurrentGameAsCheat:(BOOL)asCheat {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2GetCurrentSaveStateIdentity(&serial, &crc)) {
        NSLog(@"[ARMSX2Bridge] PNACH path unavailable: no current game identity");
        return nil;
    }

    return ARMSX2NSStringFromStdString(Patch::GetPnachFilename(serial, crc, asCheat));
}

+ (void)reloadPatches {
    std::string serial;
    u32 crc = 0;
    if (!ARMSX2GetCurrentSaveStateIdentity(&serial, &crc))
        return;

    Host::RunOnCPUThread([serial, crc]() {
        Patch::ReloadPatches(serial, crc, true, true, true, true);
        Patch::UpdateActivePatches(true, true, true, true);
    }, false);
}

#pragma mark - Memory cards

+ (nonnull NSString *)memoryCardDirectory {
    FileSystem::CreateDirectoryPath(EmuFolders::MemoryCards.c_str(), false);
    return ARMSX2NSStringFromStdString(EmuFolders::MemoryCards);
}

+ (nonnull NSArray<NSString *> *)availableMemoryCards {
    [self memoryCardDirectory];

    std::vector<AvailableMcdInfo> cards = FileMcd_GetAvailableCards(true);
    NSMutableArray<NSString *> *names = [NSMutableArray arrayWithCapacity:cards.size()];
    for (const AvailableMcdInfo& card : cards) {
        NSString* name = ARMSX2NSStringFromStdString(card.name);
        if (name.length > 0)
            [names addObject:name];
    }

    return names;
}

+ (nullable NSString *)memoryCardNameForSlot:(NSInteger)slot {
    if (slot < 1 || slot > 8)
        return nil;

    const uint nativeSlot = static_cast<uint>(slot - 1);
    char key[32];
    std::snprintf(key, sizeof(key), "Slot%u_Filename", nativeSlot + 1);

    std::string value = EmuConfig.Mcd[nativeSlot].Filename;
    if (g_p44_settings_interface)
        value = g_p44_settings_interface->GetStringValue("MemoryCards", key, value.c_str());

    return ARMSX2NSStringFromStdString(value);
}

+ (void)setMemoryCardName:(nonnull NSString *)name forSlot:(NSInteger)slot enabled:(BOOL)enabled {
    if (slot < 1 || slot > 8)
        return;

    const uint nativeSlot = static_cast<uint>(slot - 1);
    const std::string nativeName(name.UTF8String ?: "");
    char enableKey[32];
    char fileKey[32];
    std::snprintf(enableKey, sizeof(enableKey), "Slot%u_Enable", nativeSlot + 1);
    std::snprintf(fileKey, sizeof(fileKey), "Slot%u_Filename", nativeSlot + 1);

    if (g_p44_settings_interface) {
        g_p44_settings_interface->SetBoolValue("MemoryCards", enableKey, enabled);
        g_p44_settings_interface->SetStringValue("MemoryCards", fileKey, nativeName.c_str());
        g_p44_settings_interface->Save();
    }

    EmuConfig.Mcd[nativeSlot].Enabled = enabled ? true : false;
    EmuConfig.Mcd[nativeSlot].Filename = nativeName;
    if (!enabled || nativeName.empty()) {
        EmuConfig.Mcd[nativeSlot].Type = MemoryCardType::Empty;
    } else if (const std::optional<AvailableMcdInfo> cardInfo = FileMcd_GetCardInfo(nativeName)) {
        EmuConfig.Mcd[nativeSlot].Type = cardInfo->type;
    } else {
        EmuConfig.Mcd[nativeSlot].Type = MemoryCardType::File;
    }

    NSLog(@"[ARMSX2Bridge] MemoryCard slot=%ld enabled=%d name=%@", static_cast<long>(slot), enabled ? 1 : 0, name);
}

+ (BOOL)createMemoryCardNamed:(nonnull NSString *)name sizeMB:(NSInteger)sizeMB folder:(BOOL)folder {
    [self memoryCardDirectory];

    NSString* sanitized = ARMSX2SanitizedMemoryCardName(name);
    if (sanitized.length == 0)
        return NO;

    const std::string nativeName(sanitized.UTF8String ?: "");
    const std::string fullPath(Path::Combine(EmuFolders::MemoryCards, nativeName));
    if (FileSystem::FileExists(fullPath.c_str()) || FileSystem::DirectoryExists(fullPath.c_str())) {
        NSLog(@"[ARMSX2Bridge] MemoryCard create refused, already exists: %@", sanitized);
        return NO;
    }

    const MemoryCardType cardType = folder ? MemoryCardType::Folder : MemoryCardType::File;
    const MemoryCardFileType fileType = folder ? MemoryCardFileType::Unknown : ARMSX2MemoryCardFileTypeForSizeMB(sizeMB);
    if (!folder && fileType == MemoryCardFileType::Unknown)
        return NO;

    const bool result = FileMcd_CreateNewCard(nativeName, cardType, fileType);
    NSLog(@"[ARMSX2Bridge] MemoryCard create name=%@ folder=%d size=%ld result=%d",
          sanitized, folder ? 1 : 0, static_cast<long>(sizeMB), result ? 1 : 0);
    return result ? YES : NO;
}

// Gamepad button mapping
extern std::atomic<bool> s_captureMode;
extern std::atomic<int>  s_capturedButton;
extern int s_buttonMap[16];

+ (void)startButtonCapture {
    s_capturedButton.store(-1);
    s_captureMode.store(true);
}

+ (void)stopButtonCapture {
    s_captureMode.store(false);
}

// Poll SDL gamepad from main thread (for settings screen when VM is not running)
+ (void)pollGamepadForCapture {
    if (!s_captureMode.load()) return;
    SDL_UpdateGamepads();
    // Keep gamepad open across polls to avoid open/close overhead
    static SDL_Gamepad* s_settingsGP = nullptr;
    if (!s_settingsGP) {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids && count > 0) s_settingsGP = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    if (!s_settingsGP) return;
    if (!SDL_GamepadConnected(s_settingsGP)) {
        SDL_CloseGamepad(s_settingsGP);
        s_settingsGP = nullptr;
        return;
    }
    // SDL_PumpEvents required for GCController input to be processed
    SDL_PumpEvents();
    SDL_UpdateGamepads();
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
        if (SDL_GetGamepadButton(s_settingsGP, (SDL_GamepadButton)b)) {
            s_capturedButton.store(b);
            break;
        }
    }
}

+ (int)capturedButton {
    return s_capturedButton.exchange(-1);
}

+ (void)setButtonMapping:(int)ps2Index toSDLButton:(int)sdlButton {
    if (ps2Index >= 0 && ps2Index < 16) {
        s_buttonMap[ps2Index] = sdlButton;
        // Persist to INI
        if (g_p44_settings_interface) {
            char key[32];
            snprintf(key, sizeof(key), "Button%d", ps2Index);
            g_p44_settings_interface->SetIntValue("ARMSX2iOS/GamepadMapping", key, sdlButton);
            g_p44_settings_interface->Save();
        }
    }
}

+ (int)getButtonMapping:(int)ps2Index {
    if (ps2Index >= 0 && ps2Index < 16) return s_buttonMap[ps2Index];
    return -1;
}

+ (void)resetButtonMappings {
    static const int defMap[16] = {
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        -1, -1,
        SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    };
    for (int i = 0; i < 16; i++) s_buttonMap[i] = defMap[i];
    if (g_p44_settings_interface) {
        g_p44_settings_interface->RemoveSection("ARMSX2iOS/GamepadMapping");
        g_p44_settings_interface->Save();
    }
}

@end
