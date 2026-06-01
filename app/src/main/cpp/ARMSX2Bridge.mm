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
#include "Achievements.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Pad/PadDualshock2.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "Counters.h"
#include "GS/GSState.h"
#include "GameList.h"
#include "ps2/BiosTools.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ZipHelpers.h"
#include "common/Error.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <optional>
#include <string_view>
#include <vector>
#include <ifaddrs.h>
#include <net/if.h>

// Access the global settings interface from ios_main.mm
extern INISettingsInterface* g_p44_settings_interface;
extern "C" void ARMSX2_PrepareGameRenderViewForCurrentRenderer(const char* reason);
extern "C" void ARMSX2_PostRuntimeMenuStateChanged(void);
extern "C" void ARMSX2_iOSTestGamepadRumble(void);

static NSDate* s_lastNVMSaveDate = nil;

@implementation ARMSX2SaveStateSlotInfo
@end

@implementation ARMSX2BIOSInfo
@end

static NSString* const ARMSX2CompatibilityProfileOff = @"off";
static NSString* const ARMSX2CompatibilityProfileCOP1 = @"cop1";
static NSString* const ARMSX2CompatibilityProfileLoadStore = @"loadstore";
static NSString* const ARMSX2CompatibilityProfileMMI = @"mmi";
static NSString* const ARMSX2CompatibilityProfileCOP2VU = @"cop2vu";
static NSString* const ARMSX2CompatibilityProfileMultDiv = @"multdiv";
static NSString* const ARMSX2CompatibilityProfileShifts = @"shifts";
static NSString* const ARMSX2CompatibilityProfileMoves = @"moves";
static NSString* const ARMSX2CompatibilityProfileIntegerALU = @"integeralu";
static NSString* const ARMSX2CompatibilityProfileBranches = @"branches";
static NSString* const ARMSX2CompatibilityProfileCustom = @"custom";

static NSString* ARMSX2NSStringFromStdString(const std::string& value);

static NSString* ARMSX2RegionFallbackForSerial(const std::string& serial)
{
    std::string normalized;
    normalized.reserve(serial.size());
    for (const char ch : serial)
    {
        if (ch != '-' && ch != '_' && ch != ' ')
            normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }

    auto startsWith = [&normalized](const char* prefix) {
        return normalized.rfind(prefix, 0) == 0;
    };

    if (startsWith("SLUS") || startsWith("SCUS") || startsWith("PBPX"))
        return @"NTSC-U";
    if (startsWith("SLES") || startsWith("SCES") || startsWith("SLED") || startsWith("SCED"))
        return @"PAL";
    if (startsWith("SLPS") || startsWith("SLPM") || startsWith("SCPS") || startsWith("PCPX") || startsWith("SCAJ"))
        return @"NTSC-J";
    if (startsWith("SLKA") || startsWith("SCKA"))
        return @"NTSC-K";
    if (startsWith("SCCS"))
        return @"NTSC-C";
    if (startsWith("SLAJ"))
        return @"NTSC-HK";

    return nil;
}

static NSString* ARMSX2BIOSDisplayRegionForZone(NSString* zone)
{
    if ([zone isEqualToString:@"USA"])
        return @"North America";
    if ([zone length] > 0)
        return zone;
    return @"Unknown Region";
}

static NSString* ARMSX2BIOSCountryCodeForZone(NSString* zone)
{
    if ([zone isEqualToString:@"Japan"])
        return @"JP";
    if ([zone isEqualToString:@"USA"])
        return @"US";
    if ([zone isEqualToString:@"Europe"])
        return @"EU";
    if ([zone isEqualToString:@"Asia"])
        return @"HK";
    if ([zone isEqualToString:@"China"])
        return @"CN";
    return @"";
}

static ARMSX2BIOSInfo* ARMSX2MakeBIOSInfo(NSString* fileName, NSString* directory)
{
    ARMSX2BIOSInfo* info = [ARMSX2BIOSInfo new];
    info.fileName = fileName ?: @"";
    info.filePath = directory ? [directory stringByAppendingPathComponent:fileName ?: @""] : @"";
    info.regionName = @"Unknown Region";
    info.countryCode = @"";
    info.descriptionText = @"Region unavailable";
    info.regionCode = -1;
    info.valid = NO;

    u32 version = 0;
    u32 region = 0;
    std::string description;
    std::string zone;
    if (IsBIOS(info.filePath.UTF8String, version, description, region, zone)) {
        NSString* zoneString = ARMSX2NSStringFromStdString(zone);
        info.valid = YES;
        info.regionCode = static_cast<NSInteger>(region);
        info.regionName = ARMSX2BIOSDisplayRegionForZone(zoneString);
        info.countryCode = ARMSX2BIOSCountryCodeForZone(zoneString);
        info.descriptionText = ARMSX2NSStringFromStdString(description);
    }

    return info;
}

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

static NSArray<NSString*>* ARMSX2JITBisectFlagKeys()
{
    static NSArray<NSString*>* keys;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        keys = @[
            @"COP1EverythingOnly",
            @"COP1EverythingPlusLoadStore",
            @"COP1EverythingPlusMMI",
            @"COP1EverythingPlusCOP2VU",
            @"COP1EverythingPlusMultDiv",
            @"COP1EverythingPlusShifts",
            @"COP1EverythingPlusMoves",
            @"COP1EverythingPlusIntegerALU",
            @"COP1EverythingPlusBranches",
        ];
    });
    return keys;
}

static NSString* ARMSX2CompatibilityProfileFlagKey(NSString* profile)
{
    if ([profile isEqualToString:ARMSX2CompatibilityProfileCOP1]) return @"COP1EverythingOnly";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileLoadStore]) return @"COP1EverythingPlusLoadStore";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileMMI]) return @"COP1EverythingPlusMMI";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileCOP2VU]) return @"COP1EverythingPlusCOP2VU";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileMultDiv]) return @"COP1EverythingPlusMultDiv";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileShifts]) return @"COP1EverythingPlusShifts";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileMoves]) return @"COP1EverythingPlusMoves";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileIntegerALU]) return @"COP1EverythingPlusIntegerALU";
    if ([profile isEqualToString:ARMSX2CompatibilityProfileBranches]) return @"COP1EverythingPlusBranches";
    return @"";
}

static NSString* ARMSX2NormalizeCompatibilityProfile(NSString* profile)
{
    NSString* normalized = [profile.lowercaseString stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if ([normalized isEqualToString:ARMSX2CompatibilityProfileCOP1] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileLoadStore] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileMMI] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileCOP2VU] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileMultDiv] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileShifts] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileMoves] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileIntegerALU] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileBranches] ||
        [normalized isEqualToString:ARMSX2CompatibilityProfileCustom])
        return normalized;

    return ARMSX2CompatibilityProfileOff;
}

static NSString* ARMSX2CurrentCompatibilityProfileFromSettings()
{
    if (!g_p44_settings_interface)
        return ARMSX2CompatibilityProfileOff;

    std::string stored = g_p44_settings_interface->GetStringValue("ARMSX2/JITBisect", "Profile", "");
    NSString* storedProfile = ARMSX2NormalizeCompatibilityProfile(ARMSX2NSStringFromStdString(stored));
    if (![storedProfile isEqualToString:ARMSX2CompatibilityProfileOff] && ![storedProfile isEqualToString:ARMSX2CompatibilityProfileCustom])
        return storedProfile;

    NSString* activeProfile = ARMSX2CompatibilityProfileOff;
    int activeCount = 0;
    for (NSString* key in ARMSX2JITBisectFlagKeys()) {
        if (g_p44_settings_interface->GetBoolValue("ARMSX2/JITBisect", key.UTF8String, false)) {
            activeCount++;
            NSString* profile = ARMSX2CompatibilityProfileOff;
            if ([key isEqualToString:@"COP1EverythingOnly"]) profile = ARMSX2CompatibilityProfileCOP1;
            else if ([key isEqualToString:@"COP1EverythingPlusLoadStore"]) profile = ARMSX2CompatibilityProfileLoadStore;
            else if ([key isEqualToString:@"COP1EverythingPlusMMI"]) profile = ARMSX2CompatibilityProfileMMI;
            else if ([key isEqualToString:@"COP1EverythingPlusCOP2VU"]) profile = ARMSX2CompatibilityProfileCOP2VU;
            else if ([key isEqualToString:@"COP1EverythingPlusMultDiv"]) profile = ARMSX2CompatibilityProfileMultDiv;
            else if ([key isEqualToString:@"COP1EverythingPlusShifts"]) profile = ARMSX2CompatibilityProfileShifts;
            else if ([key isEqualToString:@"COP1EverythingPlusMoves"]) profile = ARMSX2CompatibilityProfileMoves;
            else if ([key isEqualToString:@"COP1EverythingPlusIntegerALU"]) profile = ARMSX2CompatibilityProfileIntegerALU;
            else if ([key isEqualToString:@"COP1EverythingPlusBranches"]) profile = ARMSX2CompatibilityProfileBranches;
            activeProfile = profile;
        }
    }

    return activeCount == 0 ? ARMSX2CompatibilityProfileOff : (activeCount == 1 ? activeProfile : ARMSX2CompatibilityProfileCustom);
}

static void ARMSX2ApplyCompatibilityProfile(NSString* profile, BOOL persistSettings, NSString* reason)
{
    NSString* normalized = ARMSX2NormalizeCompatibilityProfile(profile);
    if ([normalized isEqualToString:ARMSX2CompatibilityProfileCustom]) {
        if (persistSettings && g_p44_settings_interface) {
            g_p44_settings_interface->SetStringValue("ARMSX2/JITBisect", "Profile", normalized.UTF8String);
            g_p44_settings_interface->Save();
        }

        NSLog(@"[ARMSX2Bridge] Compatibility preset=custom reason=%@ flags preserved", reason ?: @"manual");
        return;
    }

    NSString* activeFlag = ARMSX2CompatibilityProfileFlagKey(normalized);

    for (NSString* key in ARMSX2JITBisectFlagKeys()) {
        const BOOL enabled = activeFlag.length > 0 && [key isEqualToString:activeFlag];
        ARMSX2ApplyJITBisectFlag(key, enabled);
        if (persistSettings && g_p44_settings_interface)
            g_p44_settings_interface->SetBoolValue("ARMSX2/JITBisect", key.UTF8String, enabled);
    }

    if (persistSettings && g_p44_settings_interface) {
        g_p44_settings_interface->SetStringValue("ARMSX2/JITBisect", "Profile", normalized.UTF8String);
        g_p44_settings_interface->Save();
    }

    NSLog(@"[ARMSX2Bridge] Compatibility preset=%@ reason=%@", normalized, reason ?: @"manual");
}

static NSString* ARMSX2CompatibilityCustomFlagSection(NSString* identity)
{
    return [NSString stringWithFormat:@"ARMSX2/JITBisectGamePresetFlags/%@", identity ?: @""];
}

static void ARMSX2SaveCompatibilityCustomFlagsForIdentity(NSString* identity)
{
    if (!g_p44_settings_interface || identity.length == 0)
        return;

    NSString* section = ARMSX2CompatibilityCustomFlagSection(identity);
    g_p44_settings_interface->SetStringValue("ARMSX2/JITBisectGamePresets", identity.UTF8String, ARMSX2CompatibilityProfileCustom.UTF8String);
    for (NSString* key in ARMSX2JITBisectFlagKeys()) {
        BOOL enabled = NO;
        if (int* flag = ARMSX2JITBisectFlagPtr(key))
            enabled = (*flag != 0) ? YES : NO;
        else
            enabled = g_p44_settings_interface->GetBoolValue("ARMSX2/JITBisect", key.UTF8String, false) ? YES : NO;

        g_p44_settings_interface->SetBoolValue(section.UTF8String, key.UTF8String, enabled ? true : false);
    }
    g_p44_settings_interface->Save();
    NSLog(@"[ARMSX2Bridge] Compatibility custom flags saved identity=%@", identity);
}

static BOOL ARMSX2LoadCompatibilityCustomFlagsForIdentity(NSString* identity)
{
    if (!g_p44_settings_interface || identity.length == 0)
        return NO;

    NSString* section = ARMSX2CompatibilityCustomFlagSection(identity);
    bool foundAny = false;

    for (NSString* key in ARMSX2JITBisectFlagKeys()) {
        bool enabled = false;
        if (g_p44_settings_interface->GetBoolValue(section.UTF8String, key.UTF8String, &enabled))
            foundAny = true;

        ARMSX2ApplyJITBisectFlag(key, enabled ? YES : NO);
        g_p44_settings_interface->SetBoolValue("ARMSX2/JITBisect", key.UTF8String, enabled);
    }

    if (!foundAny)
        return NO;

    g_p44_settings_interface->SetStringValue("ARMSX2/JITBisect", "Profile", ARMSX2CompatibilityProfileCustom.UTF8String);
    g_p44_settings_interface->Save();
    NSLog(@"[ARMSX2Bridge] Compatibility custom flags loaded identity=%@", identity);
    return YES;
}

static void ARMSX2ClearCompatibilityCustomFlagsForIdentity(NSString* identity)
{
    if (!g_p44_settings_interface || identity.length == 0)
        return;

    NSString* section = ARMSX2CompatibilityCustomFlagSection(identity);
    g_p44_settings_interface->ClearSection(section.UTF8String);
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

extern "C" void ARMSX2_PostRetroAchievementsStateChanged(void)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter] postNotificationName:@"ARMSX2RetroAchievementsStateChanged" object:nil];
    });
}

static dispatch_queue_t ARMSX2RetroAchievementsQueue()
{
    static dispatch_queue_t queue;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        queue = dispatch_queue_create("org.armsx2.ios.retroachievements", DISPATCH_QUEUE_SERIAL);
    });
    return queue;
}

static bool ARMSX2EnsureAchievementsClientInitialized()
{
    if (!EmuConfig.Achievements.Enabled)
        return false;

    if (!Achievements::IsActive())
        return Achievements::Initialize();

    return true;
}

static void ARMSX2SaveBaseSettingBool(const char* section, const char* key, bool value)
{
    Host::SetBaseBoolSettingValue(section, key, value);
    if (g_p44_settings_interface) {
        g_p44_settings_interface->SetBoolValue(section, key, value);
        g_p44_settings_interface->Save();
    }
}

static void ARMSX2UpdateAchievementsSettings(void (^mutate)())
{
    Pcsx2Config::AchievementsOptions old_config = EmuConfig.Achievements;
    mutate();
    Achievements::UpdateSettings(old_config);
    ARMSX2_PostRetroAchievementsStateChanged();
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

static BOOL ARMSX2PopulateGameListEntryForISO(NSString* isoName, GameList::Entry* entry, NSString** resolvedPath)
{
    NSString* path = ARMSX2ResolveISOPath(isoName);
    if (resolvedPath)
        *resolvedPath = path;

    if (path.length == 0 || !entry)
        return NO;

    return GameList::PopulateEntryFromPath(path.UTF8String, entry) ? YES : NO;
}

static NSString* ARMSX2CompatibilityIdentityKey(NSString* serial, u32 crc)
{
    NSString* normalizedSerial = [[serial ?: @"" stringByReplacingOccurrencesOfString:@"_" withString:@"-"] uppercaseString];
    normalizedSerial = [normalizedSerial stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (normalizedSerial.length > 0)
        return normalizedSerial;

    if (crc != 0)
        return [NSString stringWithFormat:@"CRC-%08X", crc];

    return @"";
}

static NSString* ARMSX2CurrentCompatibilityIdentityKey()
{
    if (!VMManager::HasValidVM())
        return @"";

    return ARMSX2CompatibilityIdentityKey(ARMSX2NSStringFromStdString(VMManager::GetDiscSerial()), VMManager::GetDiscCRC());
}

static NSString* ARMSX2CompatibilityBuiltInPreset(NSString* title, NSString* serial)
{
    NSString* haystack = [[NSString stringWithFormat:@"%@ %@", title ?: @"", serial ?: @""] lowercaseString];
    if ([haystack containsString:@"god of war"] ||
        [haystack containsString:@"budokai"] ||
        [haystack containsString:@"dragon ball"] ||
        [haystack containsString:@"naruto"])
        return ARMSX2CompatibilityProfileCOP2VU;

    return @"";
}

static NSString* ARMSX2SavedCompatibilityPreset(NSString* identity)
{
    if (!g_p44_settings_interface || identity.length == 0)
        return @"";

    std::string value = g_p44_settings_interface->GetStringValue("ARMSX2/JITBisectGamePresets", identity.UTF8String, "");
    if (value.empty())
        return @"";

    return ARMSX2NormalizeCompatibilityProfile(ARMSX2NSStringFromStdString(value));
}

static NSString* ARMSX2ResolvedCompatibilityPreset(NSString* identity, NSString* title)
{
    if (!g_p44_settings_interface)
        return ARMSX2CompatibilityProfileOff;

    const bool autoPresets = g_p44_settings_interface->GetBoolValue("ARMSX2/JITBisect", "AutoGamePresets", true);
    if (!autoPresets)
        return ARMSX2CurrentCompatibilityProfileFromSettings();

    NSString* saved = ARMSX2SavedCompatibilityPreset(identity);
    if (saved.length > 0)
        return saved;

    NSString* builtIn = ARMSX2CompatibilityBuiltInPreset(title, identity);
    if (builtIn.length > 0)
        return builtIn;

    return ARMSX2CompatibilityProfileOff;
}

static void ARMSX2ApplyCompatibilityPresetForISOName(NSString* isoName)
{
    NSString* identity = @"";
    NSString* title = isoName.stringByDeletingPathExtension ?: isoName;
    NSString* path = ARMSX2ResolveISOPath(isoName);

    if (path.length > 0) {
        GameList::Entry entry;
        if (GameList::PopulateEntryFromPath(path.UTF8String, &entry)) {
            identity = ARMSX2CompatibilityIdentityKey(ARMSX2NSStringFromStdString(entry.serial), entry.crc);
            title = ARMSX2NSStringFromStdString(entry.GetTitle(false));
            if (title.length == 0)
                title = isoName.stringByDeletingPathExtension ?: isoName;
        }
    }

    NSString* profile = ARMSX2ResolvedCompatibilityPreset(identity, title);
    if ([profile isEqualToString:ARMSX2CompatibilityProfileCustom] && ARMSX2LoadCompatibilityCustomFlagsForIdentity(identity)) {
        NSLog(@"[ARMSX2Bridge] Compatibility preset=custom identity=%@ reason=boot %@", identity ?: @"", title ?: @"");
        return;
    }
    ARMSX2ApplyCompatibilityProfile(profile, YES, [NSString stringWithFormat:@"boot %@ %@", identity ?: @"", title ?: @""]);
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

static BOOL ARMSX2IsControllerSkinImageName(NSString* name)
{
    NSString* ext = name.pathExtension.lowercaseString;
    return [ext isEqualToString:@"png"] || [ext isEqualToString:@"jpg"] ||
           [ext isEqualToString:@"jpeg"] || [ext isEqualToString:@"webp"];
}

static NSString* ARMSX2SanitizedSkinFileName(NSString* name)
{
    NSString* last = name.lastPathComponent;
    if (last.length == 0)
        return nil;

    NSMutableString* sanitized = [NSMutableString stringWithCapacity:last.length];
    NSCharacterSet* allowed = [NSCharacterSet characterSetWithCharactersInString:
        @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"];
    for (NSUInteger i = 0; i < last.length; i++) {
        unichar ch = [last characterAtIndex:i];
        [sanitized appendString:[allowed characterIsMember:ch] ? [NSString stringWithCharacters:&ch length:1] : @"_"];
    }
    return sanitized;
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
    APPLY_OSD_BOOL(DumpReplaceableTextures);
    APPLY_OSD_BOOL(DumpReplaceableMipmaps);
    APPLY_OSD_BOOL(DumpTexturesWithFMVActive);
    APPLY_OSD_BOOL(DumpDirectTextures);
    APPLY_OSD_BOOL(DumpPaletteTextures);
    APPLY_OSD_BOOL(LoadTextureReplacements);
    APPLY_OSD_BOOL(LoadTextureReplacementsAsync);
    APPLY_OSD_BOOL(PrecacheTextureReplacements);

    if (std::strcmp(key, "hw_mipmap") == 0) {
        EmuConfig.GS.HWMipmap = value;
        GSConfig.HWMipmap = value;
        return;
    }

#undef APPLY_OSD_BOOL
}

static void ARMSX2ApplyLiveGSIntSetting(const char* section, const char* key, int value)
{
    if (std::strcmp(section, "EmuCore/GS") != 0)
        return;

    if (std::strcmp(key, "OsdPerformancePos") == 0) {
        const int clamped = std::clamp(value, static_cast<int>(OsdOverlayPos::None), static_cast<int>(OsdOverlayPos::TopRight));
        EmuConfig.GS.OsdPerformancePos = static_cast<OsdOverlayPos>(clamped);
        GSConfig.OsdPerformancePos = static_cast<OsdOverlayPos>(clamped);
    } else if (std::strcmp(key, "texture_preloading") == 0) {
        const int clamped = std::clamp(value, 0, static_cast<int>(TexturePreloadingLevel::Full));
        EmuConfig.GS.TexturePreloading = static_cast<TexturePreloadingLevel>(clamped);
        GSConfig.TexturePreloading = static_cast<TexturePreloadingLevel>(clamped);
    }
}

static void ARMSX2ApplyLiveTargetSpeedSetting(std::function<void()> update, const char* section, const char* key, float value)
{
    const std::string sectionName(section ? section : "");
    const std::string keyName(key ? key : "");

    if (!VMManager::HasValidVM()) {
        update();
        NSLog(@"[ARMSX2Bridge] target speed setting stored for next boot %s/%s=%0.3f",
              sectionName.c_str(), keyName.c_str(), value);
        return;
    }

    Host::RunOnCPUThread([update = std::move(update), sectionName, keyName, value]() mutable {
        update();
        VMManager::UpdateTargetSpeed();
        NSLog(@"[ARMSX2Bridge] target speed updated on CPU thread %s/%s=%0.3f",
              sectionName.c_str(), keyName.c_str(), value);
    }, false);
}

static float ARMSX2NormalizeIOSNominalScalar(float value)
{
    return std::isfinite(value) ? std::clamp(value, 0.05f, 10.0f) : 1.0f;
}

static void ARMSX2ApplyLiveFloatSetting(const char* section, const char* key, float value)
{
    if (std::strcmp(section, "Framerate") == 0) {
        const float clamped = std::isfinite(value) ? std::clamp(value, 0.05f, 10.0f) : 1.0f;
        if (std::strcmp(key, "NominalScalar") == 0) {
            const float normalized = ARMSX2NormalizeIOSNominalScalar(value);
            if (std::fabs(normalized - clamped) > 0.001f)
                NSLog(@"[ARMSX2Bridge] clamping unsupported NominalScalar %.3f -> %.3f", clamped, normalized);
            ARMSX2ApplyLiveTargetSpeedSetting([normalized]() { EmuConfig.EmulationSpeed.NominalScalar = normalized; }, section, key, normalized);
        } else if (std::strcmp(key, "TurboScalar") == 0)
            ARMSX2ApplyLiveTargetSpeedSetting([clamped]() { EmuConfig.EmulationSpeed.TurboScalar = clamped; }, section, key, clamped);
        else if (std::strcmp(key, "SlomoScalar") == 0)
            ARMSX2ApplyLiveTargetSpeedSetting([clamped]() { EmuConfig.EmulationSpeed.SlomoScalar = clamped; }, section, key, clamped);
        else
            return;
        return;
    }

    if (std::strcmp(section, "EmuCore/GS") != 0)
        return;

    if (std::strcmp(key, "FramerateNTSC") == 0) {
        ARMSX2ApplyLiveTargetSpeedSetting([value]() { EmuConfig.GS.FramerateNTSC = value; }, section, key, value);
        return;
    }
    if (std::strcmp(key, "FrameratePAL") == 0) {
        ARMSX2ApplyLiveTargetSpeedSetting([value]() { EmuConfig.GS.FrameratePAL = value; }, section, key, value);
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

+ (void)setVMPaused:(BOOL)paused {
    if (!VMManager::HasValidVM())
        return;

    Host::RunOnCPUThread([paused]() {
        if (!VMManager::HasValidVM())
            return;

        const VMState state = VMManager::GetState();
        if (paused && state == VMState::Running) {
            VMManager::SetPaused(true);
            Console.WriteLn("@@IOS_VM_PAUSE@@ paused=1 reason=swiftui-menu");
        } else if (!paused && state == VMState::Paused) {
            VMManager::SetPaused(false);
            Console.WriteLn("@@IOS_VM_PAUSE@@ paused=0 reason=swiftui-menu");
        }
    }, false);
}

+ (void)setFullScreen:(BOOL)enabled {
    ARMSX2_SetSDLFullscreen(enabled ? true : false);
}

+ (nonnull NSString *)buildVersion {
    NSString *ver = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"?";
    return [NSString stringWithFormat:@"ARMSX2 iOS v%@", ver];
}

+ (nonnull NSArray<NSURL *> *)extractControllerSkinArchiveAtURL:(nonnull NSURL *)archiveURL
                                                    toDirectory:(nonnull NSURL *)destinationDirectory {
    NSMutableArray<NSURL *> *extracted = [NSMutableArray array];
    if (!archiveURL.isFileURL || !destinationDirectory.isFileURL)
        return extracted;

    NSError *directoryError = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtURL:destinationDirectory
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:&directoryError]) {
        NSLog(@"[ARMSX2 iOS Skins] Could not create extraction directory %@: %@",
              destinationDirectory.path, directoryError.localizedDescription);
        return extracted;
    }

    zip_error_t ze = {};
    auto zf = zip_open_managed(archiveURL.path.UTF8String, ZIP_RDONLY, &ze);
    if (!zf) {
        NSLog(@"[ARMSX2 iOS Skins] Could not open skin archive %@: %s",
              archiveURL.lastPathComponent, zip_error_strerror(&ze));
        return extracted;
    }

    const zip_int64_t count = zip_get_num_entries(zf.get(), 0);
    for (zip_uint64_t i = 0; i < static_cast<zip_uint64_t>(std::max<zip_int64_t>(count, 0)); i++) {
        zip_stat_t stat = {};
        if (zip_stat_index(zf.get(), i, ZIP_FL_ENC_GUESS, &stat) != 0 || !stat.name)
            continue;

        NSString *entryName = [NSString stringWithUTF8String:stat.name];
        if (entryName.length == 0 || [entryName hasSuffix:@"/"] || !ARMSX2IsControllerSkinImageName(entryName))
            continue;

        auto file = zip_fopen_index_managed(zf.get(), i, ZIP_FL_ENC_GUESS);
        if (!file)
            continue;

        std::optional<std::vector<u8>> data = ReadBinaryFileInZip(file.get());
        if (!data.has_value() || data->empty())
            continue;

        NSString *safeName = ARMSX2SanitizedSkinFileName(entryName);
        if (safeName.length == 0)
            continue;

        NSURL *destinationURL = [destinationDirectory URLByAppendingPathComponent:safeName];
        NSData *imageData = [NSData dataWithBytes:data->data() length:data->size()];
        if ([imageData writeToURL:destinationURL atomically:YES])
            [extracted addObject:destinationURL];
    }

    NSLog(@"[ARMSX2 iOS Skins] Extracted %lu image(s) from %@",
          static_cast<unsigned long>(extracted.count), archiveURL.lastPathComponent);
    return extracted;
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

+ (nullable NSString *)currentGameISOName {
    if (VMManager::HasValidVM()) {
        const std::string discPath = VMManager::GetDiscPath();
        if (!discPath.empty()) {
            NSString *fileName = ARMSX2NSStringFromStringView(Path::GetFileName(discPath));
            if (fileName.length > 0)
                return fileName;
        }
    }

    NSString *currentPath = [self currentISOPath];
    return currentPath.length > 0 ? currentPath.lastPathComponent : nil;
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
        NSString *region = (regionText && *regionText) ? @(regionText) : nil;
        if (!region || [region isEqualToString:@"Other"]) {
            NSString *fallbackRegion = ARMSX2RegionFallbackForSerial(entry.serial);
            if (fallbackRegion.length > 0)
                region = fallbackRegion;
        }

        if (title.length > 0)
            metadata[@"title"] = title;
        if (serial.length > 0)
            metadata[@"serial"] = serial;
        if (region.length > 0)
            metadata[@"region"] = region;
        if (entry.crc != 0)
            metadata[@"crc"] = [NSString stringWithFormat:@"%08X", entry.crc];

        NSLog(@"[ARMSX2 iOS Covers] metadata %@ title=%@ serial=%@ region=%@",
              isoName, metadata[@"title"] ?: @"", metadata[@"serial"] ?: @"", metadata[@"region"] ?: @"");
    } else {
        NSLog(@"[ARMSX2 iOS Covers] metadata unavailable %@", isoName);
    }

    return metadata;
}

+ (nonnull NSDictionary<NSString *, id> *)gameSettingsForISO:(nonnull NSString *)isoName {
    GameList::Entry entry;
    NSString* resolvedPath = nil;
    const float globalUpscale = g_p44_settings_interface ? g_p44_settings_interface->GetFloatValue("EmuCore/GS", "upscale_multiplier", 1.0f) : 1.0f;
    const std::string globalAspect = g_p44_settings_interface ? g_p44_settings_interface->GetStringValue("EmuCore/GS", "AspectRatio", "Auto 4:3/3:2") : std::string("Auto 4:3/3:2");
    const int globalTextureFiltering = g_p44_settings_interface ? g_p44_settings_interface->GetIntValue("EmuCore/GS", "filter", 2) : 2;
    const bool globalHardwareMipmapping = g_p44_settings_interface ? g_p44_settings_interface->GetBoolValue("EmuCore/GS", "hw_mipmap", true) : true;
    const int globalBlendingAccuracy = g_p44_settings_interface ? g_p44_settings_interface->GetIntValue("EmuCore/GS", "accurate_blending_unit", 1) : 1;
    const bool globalEnableCheats = g_p44_settings_interface ? g_p44_settings_interface->GetBoolValue("EmuCore", "EnableCheats", false) : false;
    const bool globalEnablePatches = g_p44_settings_interface ? g_p44_settings_interface->GetBoolValue("EmuCore", "EnablePatches", true) : true;
    const bool globalEnableGameFixes = g_p44_settings_interface ? g_p44_settings_interface->GetBoolValue("EmuCore", "EnableGameFixes", true) : true;
    const bool globalEnableGameDBHardwareFixes = g_p44_settings_interface ? !g_p44_settings_interface->GetBoolValue("EmuCore/GS", "UserHacks", false) : true;
    NSMutableDictionary<NSString*, id>* result = [@{
        @"enabled": @NO,
        @"path": @"",
        @"serial": @"",
        @"crc": @"",
        @"upscaleMultiplier": @(globalUpscale),
        @"aspectRatio": ARMSX2NSStringFromStdString(globalAspect),
        @"textureFiltering": @(globalTextureFiltering),
        @"hardwareMipmapping": @(globalHardwareMipmapping),
        @"blendingAccuracy": @(globalBlendingAccuracy),
        @"enableCheats": @(globalEnableCheats),
        @"enablePatches": @(globalEnablePatches),
        @"enableGameFixes": @(globalEnableGameFixes),
        @"enableGameDBHardwareFixes": @(globalEnableGameDBHardwareFixes),
    } mutableCopy];

    if (!ARMSX2PopulateGameListEntryForISO(isoName, &entry, &resolvedPath) || entry.crc == 0) {
        NSLog(@"[ARMSX2Bridge] Game settings unavailable for %@ path=%@", isoName, resolvedPath ?: @"");
        return result;
    }

    const std::string settingsPath = VMManager::GetGameSettingsPath(entry.serial, entry.crc);
    result[@"path"] = ARMSX2NSStringFromStdString(settingsPath);
    result[@"serial"] = ARMSX2NSStringFromStdString(entry.serial);
    result[@"crc"] = [NSString stringWithFormat:@"%08X", entry.crc];

    INISettingsInterface si(settingsPath);
    if (!si.Load())
        return result;

    const bool hasKnownOverride =
        si.GetBoolValue("ARMSX2iOS/PerGame", "Enabled", false) ||
        si.ContainsValue("EmuCore/GS", "upscale_multiplier") ||
        si.ContainsValue("EmuCore/GS", "AspectRatio") ||
        si.ContainsValue("EmuCore/GS", "filter") ||
        si.ContainsValue("EmuCore/GS", "hw_mipmap") ||
        si.ContainsValue("EmuCore/GS", "accurate_blending_unit") ||
        si.ContainsValue("EmuCore", "EnableCheats") ||
        si.ContainsValue("EmuCore", "EnablePatches") ||
        si.ContainsValue("EmuCore", "EnableGameFixes") ||
        si.ContainsValue("EmuCore/GS", "UserHacks");

    result[@"enabled"] = @(hasKnownOverride);
    NSString* currentAspect = [result[@"aspectRatio"] isKindOfClass:NSString.class] ? result[@"aspectRatio"] : @"Auto 4:3/3:2";
    result[@"upscaleMultiplier"] = @(si.GetFloatValue("EmuCore/GS", "upscale_multiplier", [result[@"upscaleMultiplier"] floatValue]));
    result[@"aspectRatio"] = ARMSX2NSStringFromStdString(si.GetStringValue("EmuCore/GS", "AspectRatio", currentAspect.UTF8String));
    result[@"textureFiltering"] = @(si.GetIntValue("EmuCore/GS", "filter", [result[@"textureFiltering"] intValue]));
    result[@"hardwareMipmapping"] = @(si.GetBoolValue("EmuCore/GS", "hw_mipmap", [result[@"hardwareMipmapping"] boolValue]));
    result[@"blendingAccuracy"] = @(si.GetIntValue("EmuCore/GS", "accurate_blending_unit", [result[@"blendingAccuracy"] intValue]));
    result[@"enableCheats"] = @(si.GetBoolValue("EmuCore", "EnableCheats", [result[@"enableCheats"] boolValue]));
    result[@"enablePatches"] = @(si.GetBoolValue("EmuCore", "EnablePatches", [result[@"enablePatches"] boolValue]));
    result[@"enableGameFixes"] = @(si.GetBoolValue("EmuCore", "EnableGameFixes", [result[@"enableGameFixes"] boolValue]));
    result[@"enableGameDBHardwareFixes"] = @(!si.GetBoolValue("EmuCore/GS", "UserHacks", ![result[@"enableGameDBHardwareFixes"] boolValue]));
    return result;
}

+ (void)setGameSettingsForISO:(nonnull NSString *)isoName
                       enabled:(BOOL)enabled
             upscaleMultiplier:(float)upscaleMultiplier
                   aspectRatio:(nonnull NSString *)aspectRatio
              textureFiltering:(int)textureFiltering
            hardwareMipmapping:(BOOL)hardwareMipmapping
              blendingAccuracy:(int)blendingAccuracy
                  enableCheats:(BOOL)enableCheats
                 enablePatches:(BOOL)enablePatches
              enableGameFixes:(BOOL)enableGameFixes
    enableGameDBHardwareFixes:(BOOL)enableGameDBHardwareFixes {
    GameList::Entry entry;
    NSString* resolvedPath = nil;
    if (!ARMSX2PopulateGameListEntryForISO(isoName, &entry, &resolvedPath) || entry.crc == 0) {
        NSLog(@"[ARMSX2Bridge] Game settings save rejected for %@ path=%@", isoName, resolvedPath ?: @"");
        return;
    }

    FileSystem::CreateDirectoryPath(EmuFolders::GameSettings.c_str(), false);

    const std::string settingsPath = VMManager::GetGameSettingsPath(entry.serial, entry.crc);
    INISettingsInterface si(settingsPath);
    si.Load();

    if (enabled) {
        si.SetBoolValue("ARMSX2iOS/PerGame", "Enabled", true);
        si.SetFloatValue("EmuCore/GS", "upscale_multiplier", upscaleMultiplier);
        si.SetStringValue("EmuCore/GS", "AspectRatio", aspectRatio.UTF8String ?: "Auto 4:3/3:2");
        si.SetIntValue("EmuCore/GS", "filter", textureFiltering);
        si.SetBoolValue("EmuCore/GS", "hw_mipmap", hardwareMipmapping);
        si.SetIntValue("EmuCore/GS", "accurate_blending_unit", blendingAccuracy);
        si.SetBoolValue("EmuCore", "EnableCheats", enableCheats);
        si.SetBoolValue("EmuCore", "EnablePatches", enablePatches);
        si.SetBoolValue("EmuCore", "EnableGameFixes", enableGameFixes);
        si.SetBoolValue("EmuCore/GS", "UserHacks", !enableGameDBHardwareFixes);
    } else {
        si.DeleteValue("ARMSX2iOS/PerGame", "Enabled");
        si.DeleteValue("EmuCore/GS", "upscale_multiplier");
        si.DeleteValue("EmuCore/GS", "AspectRatio");
        si.DeleteValue("EmuCore/GS", "filter");
        si.DeleteValue("EmuCore/GS", "hw_mipmap");
        si.DeleteValue("EmuCore/GS", "accurate_blending_unit");
        si.DeleteValue("EmuCore", "EnableCheats");
        si.DeleteValue("EmuCore", "EnablePatches");
        si.DeleteValue("EmuCore", "EnableGameFixes");
        si.DeleteValue("EmuCore/GS", "UserHacks");
        si.RemoveEmptySections();
    }

    Error error;
    const bool saved = si.Save(&error);
    NSLog(@"[ARMSX2Bridge] Game settings %@ iso=%@ serial=%@ crc=%08X path=%@ result=%d",
          enabled ? @"saved" : @"cleared", isoName, ARMSX2NSStringFromStdString(entry.serial),
          entry.crc, ARMSX2NSStringFromStdString(settingsPath), saved ? 1 : 0);
    if (!saved)
        NSLog(@"[ARMSX2Bridge] Game settings save error: %@", ARMSX2NSStringFromStdString(error.GetDescription()));
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
        bool ejectResult = false;
        bool result = false;
        Host::RunOnCPUThread([&result]() {
            const CDVD_SourceType oldSource = CDVDsys_GetSourceType();
            const std::string oldPathForLog = CDVDsys_GetFile(oldSource);
            NSLog(@"[ARMSX2Bridge] ChangeDisc eject phase oldSource=%d oldPath=%@",
                  static_cast<int>(oldSource), ARMSX2NSStringFromStdString(oldPathForLog));
            result = VMManager::ChangeDisc(CDVD_SourceType::NoDisc, {});
        }, true);
        ejectResult = result;

        [NSThread sleepForTimeInterval:1.25];

        Host::RunOnCPUThread([nativePath, &result]() {
            result = VMManager::ChangeDisc(CDVD_SourceType::Iso, nativePath);
            NSLog(@"[ARMSX2Bridge] ChangeDisc insert phase newSource=%d newPath=%@ result=%d",
                  static_cast<int>(CDVDsys_GetSourceType()),
                  ARMSX2NSStringFromStdString(CDVDsys_GetFile(CDVDsys_GetSourceType())),
                  result ? 1 : 0);
        }, true);

        if (result && g_p44_settings_interface) {
            g_p44_settings_interface->SetStringValue("GameISO", "BootISO", isoName.UTF8String);
            g_p44_settings_interface->Save();
        }

        ARMSX2_PostRuntimeMenuStateChanged();
        NSLog(@"[ARMSX2Bridge] ChangeDisc iso=%@ ejectResult=%d result=%d", isoName, ejectResult ? 1 : 0, result ? 1 : 0);
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

        ARMSX2_PostRuntimeMenuStateChanged();
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
        GSConfig.OsdShowGPU = true;
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
    ARMSX2ApplyCompatibilityPresetForISOName(isoName);
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
    NSArray<ARMSX2BIOSInfo *> *infos = [self availableBIOSInfos];
    NSMutableArray<NSString *> *bioses = [NSMutableArray arrayWithCapacity:infos.count];
    for (ARMSX2BIOSInfo *info in infos)
        [bioses addObject:info.fileName];
    return bioses;
}

+ (nonnull NSArray<ARMSX2BIOSInfo *> *)availableBIOSInfos {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSMutableSet *seen = [NSMutableSet set];
    NSMutableArray<ARMSX2BIOSInfo *> *bioses = [NSMutableArray array];

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
                    [bioses addObject:ARMSX2MakeBIOSInfo(file, dir)];
                    [seen addObject:file];
                }
            }
        }
    };

    scanDir([self biosDirectory]);
    [bioses sortUsingComparator:^NSComparisonResult(ARMSX2BIOSInfo *lhs, ARMSX2BIOSInfo *rhs) {
        return [lhs.fileName localizedCaseInsensitiveCompare:rhs.fileName];
    }];
    return bioses;
}

+ (nonnull ARMSX2BIOSInfo *)biosInfoForName:(nonnull NSString *)biosName {
    return ARMSX2MakeBIOSInfo(biosName, [self biosDirectory]);
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
    float valueToStore = value;
    if (std::strcmp(section.UTF8String, "Framerate") == 0 && std::strcmp(key.UTF8String, "NominalScalar") == 0)
        valueToStore = ARMSX2NormalizeIOSNominalScalar(value);

    g_p44_settings_interface->SetFloatValue(section.UTF8String, key.UTF8String, valueToStore);
    g_p44_settings_interface->Save();
    ARMSX2ApplyLiveFloatSetting(section.UTF8String, key.UTF8String, valueToStore);
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
        g_p44_settings_interface->SetStringValue("ARMSX2/JITBisect", "Profile", ARMSX2CompatibilityProfileCustom.UTF8String);
        g_p44_settings_interface->Save();
    }
    NSString* identity = ARMSX2CurrentCompatibilityIdentityKey();
    if (identity.length > 0)
        ARMSX2SaveCompatibilityCustomFlagsForIdentity(identity);
    NSLog(@"[ARMSX2Bridge] Compatibility Lab %@ %@", key, value ? @"ON" : @"OFF");
}

+ (nonnull NSString *)compatibilityPresetForCurrentGame
{
    return ARMSX2CurrentCompatibilityProfileFromSettings();
}

+ (nonnull NSString *)compatibilityIdentityForCurrentGame
{
    return ARMSX2CurrentCompatibilityIdentityKey();
}

+ (BOOL)isCompatibilityAutoGamePresetsEnabled
{
    if (!g_p44_settings_interface)
        return YES;
    return g_p44_settings_interface->GetBoolValue("ARMSX2/JITBisect", "AutoGamePresets", true) ? YES : NO;
}

+ (void)setCompatibilityAutoGamePresetsEnabled:(BOOL)enabled
{
    if (g_p44_settings_interface) {
        g_p44_settings_interface->SetBoolValue("ARMSX2/JITBisect", "AutoGamePresets", enabled ? true : false);
        g_p44_settings_interface->Save();
    }
    NSLog(@"[ARMSX2Bridge] Compatibility auto game presets %@", enabled ? @"ON" : @"OFF");
}

+ (void)setCompatibilityPreset:(nonnull NSString *)preset rememberForCurrentGame:(BOOL)rememberForCurrentGame
{
    NSString* normalized = ARMSX2NormalizeCompatibilityProfile(preset);
    ARMSX2ApplyCompatibilityProfile(normalized, YES, rememberForCurrentGame ? @"remember current game" : @"manual preset");

    if (rememberForCurrentGame && g_p44_settings_interface) {
        NSString* identity = ARMSX2CurrentCompatibilityIdentityKey();
        if (identity.length > 0) {
            g_p44_settings_interface->SetStringValue("ARMSX2/JITBisectGamePresets", identity.UTF8String, normalized.UTF8String);
            g_p44_settings_interface->Save();
            NSLog(@"[ARMSX2Bridge] Compatibility remembered preset=%@ identity=%@", normalized, identity);
        }
    }
}

+ (void)forgetCompatibilityPresetForCurrentGame
{
    if (!g_p44_settings_interface)
        return;

    NSString* identity = ARMSX2CurrentCompatibilityIdentityKey();
    if (identity.length == 0)
        return;

    g_p44_settings_interface->DeleteValue("ARMSX2/JITBisectGamePresets", identity.UTF8String);
    ARMSX2ClearCompatibilityCustomFlagsForIdentity(identity);
    g_p44_settings_interface->Save();
    NSString* profile = ARMSX2ResolvedCompatibilityPreset(identity, identity);
    ARMSX2ApplyCompatibilityProfile(profile, YES, [NSString stringWithFormat:@"forget %@", identity]);
    NSLog(@"[ARMSX2Bridge] Compatibility forgot preset identity=%@", identity);
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

+ (void)resetVM {
    if (!VMManager::HasValidVM()) {
        NSLog(@"[ARMSX2Bridge] Reset VM rejected: no valid VM");
        return;
    }

    Host::RunOnCPUThread([]() {
        if (!VMManager::HasValidVM())
            return;

        NSLog(@"[ARMSX2Bridge] Reset VM requested");
        VMManager::Reset();
    }, false);
}

+ (void)testControllerRumble {
    ARMSX2_iOSTestGamepadRumble();
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

+ (nullable NSString *)pnachPathForISO:(nonnull NSString *)isoName asCheat:(BOOL)asCheat {
    NSString* path = ARMSX2ResolveISOPath(isoName);
    if (path.length == 0) {
        NSLog(@"[ARMSX2Bridge] PNACH path unavailable for %@: ISO not found", isoName);
        return nil;
    }

    GameList::Entry entry;
    if (!GameList::PopulateEntryFromPath(path.UTF8String, &entry) || entry.crc == 0) {
        NSLog(@"[ARMSX2Bridge] PNACH path unavailable for %@: metadata missing", isoName);
        return nil;
    }

    return ARMSX2NSStringFromStdString(Patch::GetPnachFilename(entry.serial, entry.crc, asCheat));
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

#pragma mark - DEV9 / Network

+ (nonnull NSArray<NSString *> *)dev9NetworkAdapters {
    NSMutableOrderedSet<NSString *> *adapters = [NSMutableOrderedSet orderedSetWithObject:@"Auto"];

    struct ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (struct ifaddrs *ifa = interfaces; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || !ifa->ifa_addr)
                continue;

            const sa_family_t family = ifa->ifa_addr->sa_family;
            if (family != AF_INET)
                continue;

            if ((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0)
                continue;

            [adapters addObject:[NSString stringWithUTF8String:ifa->ifa_name]];
        }
        freeifaddrs(interfaces);
    }

    return adapters.array;
}

#pragma mark - RetroAchievements

+ (nonnull NSDictionary<NSString *, id> *)retroAchievementsState {
    Achievements::UserStats userStats;
    Achievements::GameStats gameStats;
    bool loggedIn = false;
    bool hasGame = false;
    bool active = false;
    bool hardcoreActive = false;

    {
        auto lock = Achievements::GetLock();
        active = Achievements::IsActive();
        loggedIn = Achievements::GetCurrentUserStats(&userStats);
        hasGame = Achievements::GetCurrentGameStats(&gameStats);
        hardcoreActive = Achievements::IsHardcoreModeActive();
    }

    return @{
        @"enabled": @(EmuConfig.Achievements.Enabled),
        @"active": @(active),
        @"loggedIn": @(loggedIn),
        @"username": ARMSX2NSStringFromStdString(userStats.username),
        @"displayName": ARMSX2NSStringFromStdString(userStats.display_name),
        @"avatarPath": ARMSX2NSStringFromStdString(userStats.avatar_path),
        @"points": @(userStats.points),
        @"softcorePoints": @(userStats.softcore_points),
        @"unreadMessages": @(userStats.unread_messages),
        @"hardcorePreference": @(EmuConfig.Achievements.HardcoreMode),
        @"hardcoreActive": @(hardcoreActive),
        @"notifications": @(EmuConfig.Achievements.Notifications),
        @"leaderboardNotifications": @(EmuConfig.Achievements.LeaderboardNotifications),
        @"overlays": @(EmuConfig.Achievements.Overlays),
        @"hasActiveGame": @(hasGame),
        @"gameTitle": ARMSX2NSStringFromStdString(gameStats.title),
        @"richPresence": ARMSX2NSStringFromStdString(gameStats.rich_presence),
        @"gameIconPath": ARMSX2NSStringFromStdString(gameStats.icon_path),
        @"gameIconURL": ARMSX2NSStringFromStdString(gameStats.icon_url),
        @"unlockedAchievements": @(gameStats.unlocked_achievements),
        @"totalAchievements": @(gameStats.total_achievements),
        @"unlockedPoints": @(gameStats.unlocked_points),
        @"totalPoints": @(gameStats.total_points),
        @"gameId": @(gameStats.game_id),
        @"hasAchievements": @(gameStats.has_achievements),
        @"hasLeaderboards": @(gameStats.has_leaderboards),
        @"hasRichPresence": @(gameStats.has_rich_presence),
    };
}

+ (void)setRetroAchievementsEnabled:(BOOL)enabled {
    const bool enable = enabled ? true : false;
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        if (EmuConfig.Achievements.Enabled == enable) {
            ARMSX2SaveBaseSettingBool("Achievements", "Enabled", enable);
            ARMSX2_PostRetroAchievementsStateChanged();
            return;
        }

        ARMSX2UpdateAchievementsSettings(^{
            EmuConfig.Achievements.Enabled = enable;
            ARMSX2SaveBaseSettingBool("Achievements", "Enabled", enable);
        });
        NSLog(@"[ARMSX2Bridge] RetroAchievements enabled=%d", enable ? 1 : 0);
    });
}

+ (void)setRetroAchievementsHardcore:(BOOL)enabled {
    const bool enable = enabled ? true : false;
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        if (EmuConfig.Achievements.HardcoreMode == enable) {
            ARMSX2SaveBaseSettingBool("Achievements", "ChallengeMode", enable);
            ARMSX2_PostRetroAchievementsStateChanged();
            return;
        }

        ARMSX2UpdateAchievementsSettings(^{
            EmuConfig.Achievements.HardcoreMode = enable;
            ARMSX2SaveBaseSettingBool("Achievements", "ChallengeMode", enable);
        });
        NSLog(@"[ARMSX2Bridge] RetroAchievements hardcore=%d", enable ? 1 : 0);
    });
}

+ (void)setRetroAchievementsNotifications:(BOOL)enabled {
    const bool enable = enabled ? true : false;
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        ARMSX2UpdateAchievementsSettings(^{
            EmuConfig.Achievements.Notifications = enable;
            ARMSX2SaveBaseSettingBool("Achievements", "Notifications", enable);
        });
    });
}

+ (void)setRetroAchievementsLeaderboards:(BOOL)enabled {
    const bool enable = enabled ? true : false;
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        ARMSX2UpdateAchievementsSettings(^{
            EmuConfig.Achievements.LeaderboardNotifications = enable;
            ARMSX2SaveBaseSettingBool("Achievements", "LeaderboardNotifications", enable);
        });
    });
}

+ (void)setRetroAchievementsOverlays:(BOOL)enabled {
    const bool enable = enabled ? true : false;
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        ARMSX2UpdateAchievementsSettings(^{
            EmuConfig.Achievements.Overlays = enable;
            ARMSX2SaveBaseSettingBool("Achievements", "Overlays", enable);
        });
    });
}

+ (void)loginRetroAchievementsWithUsername:(nonnull NSString *)username password:(nonnull NSString *)password completion:(nullable ARMSX2RetroAchievementsCompletion)completion {
    NSString* trimmedUsername = [username stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    NSString* nativePassword = password ?: @"";
    ARMSX2RetroAchievementsCompletion callback = [completion copy];

    if (trimmedUsername.length == 0 || nativePassword.length == 0) {
        if (callback)
            dispatch_async(dispatch_get_main_queue(), ^{ callback(NO, @"Enter your RetroAchievements username and password."); });
        return;
    }

    std::string user(trimmedUsername.UTF8String ?: "");
    std::string pass(nativePassword.UTF8String ?: "");

    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        @autoreleasepool {
            if (!EmuConfig.Achievements.Enabled) {
                Pcsx2Config::AchievementsOptions old_config = EmuConfig.Achievements;
                EmuConfig.Achievements.Enabled = true;
                ARMSX2SaveBaseSettingBool("Achievements", "Enabled", true);
                Achievements::UpdateSettings(old_config);
            }

            if (!ARMSX2EnsureAchievementsClientInitialized()) {
                NSString* message = @"RetroAchievements could not initialize its network client.";
                NSLog(@"[ARMSX2Bridge] RetroAchievements login username=%@ result=0 message=%@", trimmedUsername, message);
                ARMSX2_PostRetroAchievementsStateChanged();
                if (callback)
                    dispatch_async(dispatch_get_main_queue(), ^{ callback(NO, message); });
                return;
            }

            Error error;
            const bool result = Achievements::Login(user.c_str(), pass.c_str(), &error);
            NSString* message = result ? @"RetroAchievements login successful." :
                (error.IsValid() ? ARMSX2NSStringFromStdString(error.GetDescription()) : @"RetroAchievements login failed.");

            NSLog(@"[ARMSX2Bridge] RetroAchievements login username=%@ result=%d message=%@", trimmedUsername, result ? 1 : 0, message);
            ARMSX2_PostRetroAchievementsStateChanged();

            if (callback)
                dispatch_async(dispatch_get_main_queue(), ^{ callback(result ? YES : NO, message); });
        }
    });
}

+ (void)logoutRetroAchievements {
    dispatch_async(ARMSX2RetroAchievementsQueue(), ^{
        Achievements::Logout();
        if (g_p44_settings_interface)
            g_p44_settings_interface->Save();
        NSLog(@"[ARMSX2Bridge] RetroAchievements logout");
        ARMSX2_PostRetroAchievementsStateChanged();
    });
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
