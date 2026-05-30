// ARMSX2Bridge.h — ObjC bridge for C++ emulator control
// SPDX-License-Identifier: GPL-3.0+

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

typedef NS_ENUM(NSInteger, ARMSX2EmulatorState) {
    ARMSX2EmulatorStateStopped = 0,
    ARMSX2EmulatorStateRunning,
    ARMSX2EmulatorStatePaused,
    ARMSX2EmulatorStateSaving,
    ARMSX2EmulatorStateSuspended,
};

typedef NS_ENUM(NSInteger, ARMSX2CoreType) {
    ARMSX2CoreTypeLegacyRecompiler = 0,
    ARMSX2CoreTypeInterpreter = 1,
    ARMSX2CoreTypeARM64JIT = 2,
    ARMSX2CoreTypeJIT = ARMSX2CoreTypeARM64JIT,
};

typedef NS_ENUM(NSInteger, ARMSX2PadButton) {
    ARMSX2PadButtonUp = 0,
    ARMSX2PadButtonDown,
    ARMSX2PadButtonLeft,
    ARMSX2PadButtonRight,
    ARMSX2PadButtonCross,
    ARMSX2PadButtonCircle,
    ARMSX2PadButtonSquare,
    ARMSX2PadButtonTriangle,
    ARMSX2PadButtonL1,
    ARMSX2PadButtonR1,
    ARMSX2PadButtonL2,
    ARMSX2PadButtonR2,
    ARMSX2PadButtonStart,
    ARMSX2PadButtonSelect,
    ARMSX2PadButtonL3,
    ARMSX2PadButtonR3,
};

@interface ARMSX2SaveStateSlotInfo : NSObject
@property (nonatomic, assign) NSInteger slot;
@property (nonatomic, assign) BOOL occupied;
@property (nonatomic, copy, nonnull) NSString *filePath;
@property (nonatomic, copy, nonnull) NSString *fileName;
@property (nonatomic, strong, nullable) NSDate *modifiedDate;
@property (nonatomic, strong, nullable) NSData *previewPNGData;
@end

typedef void (^ARMSX2SaveStateCompletion)(BOOL success);

@interface ARMSX2Bridge : NSObject

// Game render view (for UIViewRepresentable)
+ (nonnull UIView *)gameRenderView;
+ (void)prepareGameRenderViewForCurrentRenderer;

// Lifecycle
+ (void)saveNVRAM;
+ (void)saveMemoryCards;
+ (void)saveAllState;  // NVM + MC
+ (BOOL)isRunning;

// NVM status
+ (nullable NSDate *)lastNVMSaveDate;
+ (nullable NSString *)nvmFilePath;
+ (BOOL)nvmFileExists;

// Pad input
+ (void)setPadButton:(ARMSX2PadButton)button pressed:(BOOL)pressed;
+ (void)setLeftStickX:(float)x Y:(float)y;
+ (void)setRightStickX:(float)x Y:(float)y;

// VM control
+ (void)requestVMStop;
+ (void)setFullScreen:(BOOL)enabled;

// Info
+ (nonnull NSString *)biosName;
+ (nonnull NSString *)buildVersion;

// OSD overlay
+ (void)setPerformanceOverlayVisible:(BOOL)visible;
+ (BOOL)isPerformanceOverlayVisible;
+ (void)applyOsdPreset:(int)preset;  // 0=off, 1=simple, 2=detail, 3=full

// ISO management
+ (nullable NSString *)currentISOPath;
+ (nonnull NSString *)isoDirectory;
+ (nonnull NSString *)documentsDirectory;
+ (nonnull NSArray<NSString *> *)availableISOs;
+ (nonnull NSDictionary<NSString *, NSString *> *)gameMetadataForISO:(nonnull NSString *)isoName;

// [P44] ISO boot
+ (void)bootISO:(nonnull NSString *)isoName;

// [P44] BIOS management
+ (nonnull NSString *)biosDirectory;
+ (nonnull NSArray<NSString *> *)availableBIOSes;
+ (nonnull NSString *)defaultBIOSName;
+ (void)setDefaultBIOS:(nonnull NSString *)biosName;

// [P44] Favorites
+ (BOOL)isFavorite:(nonnull NSString *)isoName;
+ (void)setFavorite:(nonnull NSString *)isoName favorite:(BOOL)favorite;

// [P44] INI generic getter/setter
+ (int)getINIInt:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(int)def;
+ (BOOL)getINIBool:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(BOOL)def;
+ (float)getINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(float)def;
+ (nonnull NSString *)getINIString:(nonnull NSString *)section key:(nonnull NSString *)key defaultValue:(nonnull NSString *)def;
+ (void)setINIInt:(nonnull NSString *)section key:(nonnull NSString *)key value:(int)value;
+ (void)setINIBool:(nonnull NSString *)section key:(nonnull NSString *)key value:(BOOL)value;
+ (void)setINIFloat:(nonnull NSString *)section key:(nonnull NSString *)key value:(float)value;
+ (void)setINIString:(nonnull NSString *)section key:(nonnull NSString *)key value:(nonnull NSString *)value;

// [P44] VM lifecycle for menu flow
+ (BOOL)isVMRunning;
+ (BOOL)hasBIOS;
+ (void)requestVMBoot;
+ (void)requestVMShutdown;

// Save states
+ (BOOL)hasValidSaveStateGame;
+ (nonnull NSArray<ARMSX2SaveStateSlotInfo *> *)saveStateSlots;
+ (void)saveStateToSlot:(NSInteger)slot completion:(nullable ARMSX2SaveStateCompletion)completion NS_SWIFT_NAME(saveState(toSlot:completion:));
+ (void)loadStateFromSlot:(NSInteger)slot completion:(nullable ARMSX2SaveStateCompletion)completion NS_SWIFT_NAME(loadState(fromSlot:completion:));

// [P53] Gamepad button mapping
+ (void)startButtonCapture;
+ (void)stopButtonCapture;
+ (void)pollGamepadForCapture;  // call from main thread when VM is not running
+ (int)capturedButton;  // returns SDL_GamepadButton or -1
+ (void)setButtonMapping:(int)ps2Index toSDLButton:(int)sdlButton;
+ (int)getButtonMapping:(int)ps2Index;
+ (void)resetButtonMappings;

@end
