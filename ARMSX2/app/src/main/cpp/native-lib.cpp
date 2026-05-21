#include <jni.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include "PrecompiledHeader.h"
#include "tests/arm64/run_tests.h"
#include "tests/core/run_patch_tests.h"
#include "tests/mvu/run_mvu_tests.h"
#include "tests/ee/run_ee_tests.h"
#include "tests/ee/run_ee_seq_tests.h"
#include "tests/vif/run_vif_tests.h"
#include "common/StringUtil.h"
#include "common/FileSystem.h"
#include "common/ZipHelpers.h"
#include "pcsx2/GS.h"
#include "pcsx2/VMManager.h"
#include "PerformanceMetrics.h"
#include "GameList.h"
#include "GameDatabase.h"
#include "GS/GSPerfMon.h"
#include "GS/Renderers/Vulkan/VKShaderCache.h"
#include "GSDumpReplayer.h"
#include "ImGui/ImGuiManager.h"
#include "common/Path.h"
#include "common/MemorySettingsInterface.h"
#include "pcsx2/INISettingsInterface.h"
#include "SIO/Pad/Pad.h"
#include "Input/InputManager.h"
#include "ImGui/ImGuiFullscreen.h"
#include "Achievements.h"
#include "common/Error.h"
#include "common/HTTPDownloaderAndroid.h"
#include "Host.h"
#include "ImGui/FullscreenUI.h"
#include "SIO/Pad/PadDualshock2.h"
#include "MTGS.h"
#include "GS/Renderers/Vulkan/VKLoader.h"
#include "SDL3/SDL.h"
#include "ps2/BiosTools.h"
#include "BuildVersion.h"
#include "native-lib.h"
#include "libchdr/chd.h"
#include <algorithm>
#include <cctype>
#include <future>
#include <functional>
#include <regex>
#include <vector>


// Redirect stdout/stderr to Android logcat so Vixl/libc abort messages are visible.
static void* stdout_redirect_thread(void* fd_ptr)
{
    int fd = (int)(intptr_t)fd_ptr;
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0)
    {
        buf[n] = '\0';
        __android_log_print(ANDROID_LOG_WARN, "STDOUT", "%s", buf);
    }
    close(fd);
    return nullptr;
}
static void redirect_stdout_to_logcat()
{
    int pfds[2];
    if (pipe(pfds) != 0) return;
    dup2(pfds[1], STDOUT_FILENO);
    dup2(pfds[1], STDERR_FILENO);
    close(pfds[1]);
    pthread_t t;
    pthread_create(&t, nullptr, stdout_redirect_thread, (void*)(intptr_t)pfds[0]);
    pthread_detach(t);
}

bool s_execute_exit;
int s_window_width = 0;
int s_window_height = 0;
ANativeWindow* s_window = nullptr;

static MemorySettingsInterface s_settings_interface;
// File-backed RetroAchievements credentials store. Holds Token (written by
// rcheevos at Achievements.cpp:2018) AND Username (mirrored from BASE on
// login so it survives restart — BASE itself is the in-memory
// `s_settings_interface` and resets every launch). Path resolved in
// Java_..._initialize once EmuFolders::DataRoot is known. Lazy-constructed
// std::unique_ptr because INISettingsInterface needs a path at construction.
static std::unique_ptr<INISettingsInterface> s_secrets_settings_interface;

static JNIEnv env_main;

// Cached JVM + refs for callbacks originating on non-Java threads (e.g. vmSetPaused).
// Populated once in initialize() while we have a valid Java-thread env.
static JavaVM*    s_jvm              = nullptr;
static jclass     s_NativeApp_class  = nullptr;  // GlobalRef
static jmethodID  s_vmSetPaused_mid  = nullptr;

////
std::string GetJavaString(JNIEnv *env, jstring jstr) {
    if (!jstr) {
        return "";
    }
    const char *str = env->GetStringUTFChars(jstr, nullptr);
    std::string cpp_string = std::string(str);
    env->ReleaseStringUTFChars(jstr, str);
    return cpp_string;
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_initialize(JNIEnv *env, jclass clazz,
                                                jstring p_szpath,
                                                jstring p_szbiosfolder,
                                                jint p_apiVer) {
    redirect_stdout_to_logcat();
    // p_szpath is the user's chosen system folder (memcards, savestates,
    // configs land here) when set up via the wizard; falls back to the
    // app's externalFilesDir when unset. p_szbiosfolder is always the
    // app's externalFilesDir/bios — the wizard copies the user-picked
    // BIOS there via private File APIs, which the chosen-systemDir path
    // can't necessarily host on Android 11+ scoped storage. Pinning
    // Folders/Bios separately keeps BIOS loading working regardless of
    // where DataRoot points.
    std::string _szPath = GetJavaString(env, p_szpath);
    std::string _szBiosFolder = GetJavaString(env, p_szbiosfolder);
    EmuFolders::AppRoot = _szPath;
    EmuFolders::DataRoot = _szPath;
    EmuFolders::SetResourcesDirectory();

    Log::SetConsoleOutputLevel(LOGLEVEL_DEBUG);
    // Font loading is handled by ImGuiManager::LoadFontData() using s_font_path fallback

    bool _SettingsIsEmpty = s_settings_interface.IsEmpty();
    if(_SettingsIsEmpty) {
        // don't provide an ini path, or bother loading. we'll store everything in memory.
        MemorySettingsInterface &si = s_settings_interface;
        Host::Internal::SetBaseSettingsLayer(&si);

        // Build the secrets layer file-backed at <DataRoot>/achievements.ini.
        // Persists the RetroAchievements auth token across app launches so
        // the user doesn't have to log in every cold start. Path::Combine
        // handles the trailing-slash form for both system and app-private
        // DataRoot. Load() returns false when the file doesn't exist yet
        // (first launch) — that's fine, the file gets created on first
        // Save() inside ClientLoginWithPasswordCallback.
        const std::string secrets_path =
            Path::Combine(EmuFolders::DataRoot, "achievements.ini");
        s_secrets_settings_interface =
            std::make_unique<INISettingsInterface>(secrets_path);
        s_secrets_settings_interface->Load();
        Host::Internal::SetSecretsSettingsLayer(s_secrets_settings_interface.get());

        VMManager::SetDefaultSettings(si, true, true, true, true, true);

        // Mirror Username from secrets → BASE so Achievements::Initialize's
        // GetBaseStringSettingValue("Achievements","Username") finds it on
        // a returning user. We co-store Username in achievements.ini at
        // login time (see Java_..._loginAchievements below) — without this
        // re-push, Initialize would have a Token but no Username, fail the
        // username.empty() guard at Achievements.cpp:608, and skip the
        // token-relogin path. User would still be "logged in" per the
        // overlay panel (which falls back to settings layer reads), but
        // s_client wouldn't be bound and game-side achievement loading
        // wouldn't fire.
        const std::string saved_user = s_secrets_settings_interface->GetStringValue(
            "Achievements", "Username", "");
        if (!saved_user.empty())
            Host::SetBaseStringSettingValue("Achievements", "Username", saved_user.c_str());

        // FrameLimitEnable is inert in this fork (no read site outside a
        // commented MTGS check). Frame pacing is driven by SetLimiterMode at
        // runtime; the persisted bool is applied after Initialize succeeds in
        // runVMThread below. Don't pre-force it here — that just confuses the
        // overlay's saved-state display.
        si.SetIntValue("EmuCore/GS", "VsyncEnable", false);
        si.SetBoolValue("EmuCore", "EnableThreadPinning", true);
        si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableFastmem", true);

        // ensure all input sources are disabled, we're not using them
        si.SetBoolValue("InputSources", "SDL", true);
        si.SetBoolValue("InputSources", "XInput", false);

        si.SetStringValue("SPU2/Output", "Backend", "Oboe");
        si.SetBoolValue("EmuCore", "EnableFastBoot", false);

        // Enable RetroAchievements by default. Pcsx2Config defaults this to
        // false (privacy-conscious for desktop), but on Android the in-game
        // overlay's right-side panel + login form make it discoverable, and
        // without Enabled=true, Achievements::Initialize never runs →
        // s_client stays null → s_has_achievements stays false → the panel
        // permanently shows "No achievements" even with a logged-in user
        // and a recognised game. Users who don't want RA can flip it off
        // via a future settings toggle (or env override).
        si.SetBoolValue("Achievements", "Enabled", true);

        // Pin BIOS folder to the app's externalFilesDir/bios regardless
        // of where the user pointed DataRoot. The setup wizard's
        // finishBiosStep copies the chosen BIOS file there via java.io
        // (always writable), and this absolute path bypasses the
        // DataRoot/bios default that EmuFolders::LoadConfig would
        // compute otherwise. Path::Combine treats absolute second args
        // as-is, so EmuFolders::Bios resolves directly to this folder.
        if (!_szBiosFolder.empty())
            si.SetStringValue("Folders", "Bios", _szBiosFolder.c_str());

        // Renderer is left at Auto (Pcsx2Config::DEFAULT_HW_RENDERER) so
        // GSUtil::GetPreferredRenderer chooses at runtime — on Android that
        // resolves to OpenGL HW. SW + VK can still be picked via the
        // RenderModeButton (cycles VULKAN_SW ↔ OPENGL). VK HW is intentionally
        // not in the cycle while its blending bugs remain unresolved.

        // OpenGL HW: leave texture barriers on Auto (-1).
        //
        // With the Mali GPU profile restored (see GSGPUProfile + the Mali
        // block in GSDeviceOGL::CheckFeatures), Auto is the correct default:
        //   - Mali devices that report GL_ARM_shader_framebuffer_fetch use
        //     that as the texture-barrier substitute. Forcing `1` here
        //     skipped the Mali Auto branch and installed
        //     MemoryBarrierAsTextureBarrier instead, which is the wrong
        //     path for Mali.
        //   - Adreno + other GLES devices still fall through to the
        //     multidraw_fb_copy fallback (or the ARB barrier path on
        //     desktop) without the override.
        //
        // The earlier `= 1` (Force Enabled) was a diagnostic experiment for
        // SH2 pop-in. If that regression returns under Auto, expose the
        // override as a per-user toggle in the Renderer tab rather than
        // forcing it globally.
        si.SetIntValue("EmuCore/GS", "OverrideTextureBarriers", -1);

        // none of the bindings are going to resolve to anything
        Pad::ClearPortBindings(si, 0);
        si.ClearSection("Hotkeys");

        // force logging
        //si.SetBoolValue("Logging", "EnableSystemConsole", !s_no_console);
        si.SetBoolValue("Logging", "EnableSystemConsole", true);
        si.SetBoolValue("Logging", "EnableTimestamps", true);
        si.SetBoolValue("Logging", "EnableVerbose", true);

        // and show some stats :)
        si.SetBoolValue("EmuCore/GS", "OsdShowFPS", true);
        si.SetBoolValue("EmuCore/GS", "OsdShowSpeed", true);
        si.SetBoolValue("EmuCore/GS", "OsdShowResolution", true);
        si.SetBoolValue("EmuCore/GS", "OsdShowCPU", true);
        si.SetBoolValue("EmuCore/GS", "OsdShowGPU", true);
        si.SetBoolValue("EmuCore/GS", "OsdShowGSStats", true);
        si.SetBoolValue("EmuCore/GS", "OsdShowFrameTimes", true);
        si.SetBoolValue("EmuCore/GS", "OsdShowHardwareInfo", true);
        si.SetBoolValue("EmuCore/GS", "OsdShowVersion", true);
        si.SetBoolValue("EmuCore/GS", "OsdShowSettings", true);
        si.SetBoolValue("EmuCore/GS", "OsdShowInputs", true);
//        // remove memory cards, so we don't have sharing violations
//        for (u32 i = 0; i < 2; i++)
//        {
//            si.SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", i + 1).c_str(), false);
//            si.SetStringValue("MemoryCards", fmt::format("Slot{}_Filename", i + 1).c_str(), "");
//        }
    }

    VMManager::Internal::LoadStartupSettings();

    // Cache JavaVM + NativeApp refs for use from non-Java threads.
    env->GetJavaVM(&s_jvm);
    if (jclass local = env->FindClass("kr/co/iefriends/pcsx2/NativeApp")) {
        s_NativeApp_class = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        s_vmSetPaused_mid = env->GetStaticMethodID(s_NativeApp_class, "vmSetPaused", "(Z)V");
    }

    // Bind the JNI-backed HTTP downloader's class + method IDs while we
    // still have a Java-thread env. Worker threads spawned from
    // HTTPDownloaderAndroid::StartRequest don't have a class loader, so
    // FindClass would fail there — these globals must be cached up front.
    HTTPDownloaderAndroid::BindFromJNI(env);
}

extern "C"
JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getGameTitle(JNIEnv *env, jclass clazz,
                                                  jstring p_szpath) {
    std::string _szPath = GetJavaString(env, p_szpath);

    const GameList::Entry *entry;
    entry = GameList::GetEntryForPath(_szPath.c_str());

    std::string ret;
    ret.append(entry->title);
    ret.append("|");
    ret.append(entry->serial);
    ret.append("|");
    ret.append(StringUtil::StdStringFromFormat("%s (%08X)", entry->serial.c_str(), entry->crc));

    return env->NewStringUTF(ret.c_str());
}

extern "C"
JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getGameSerial(JNIEnv *env, jclass clazz) {
    std::string ret = VMManager::GetDiscSerial();
    return env->NewStringUTF(ret.c_str());
}

// Build version string sourced from BuildVersion::GitRev. Format:
//   "GitTagHi.GitTagMid.GitTagLo.ARMSX2Build-SNAPSHOT"
// Used by the setup wizard + in-game overlay to show the build label
// without hardcoding the values on the Kotlin side.
extern "C"
JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getBuildVersion(JNIEnv *env, jclass clazz) {
    return env->NewStringUTF(BuildVersion::GitRev);
}

// Achievements snapshot for the in-game overlay's right-side panel.
// Format documented at Achievements::GetAchievementsAsJSON. Empty payload
// (`{"active":false,"loggedIn":false,"userName":"","items":[]}`) when no
// active game / no client / not logged in.
extern "C"
JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getAchievementsJSON(JNIEnv *env, jclass clazz) {
    const std::string json = Achievements::GetAchievementsAsJSON();
    return env->NewStringUTF(json.c_str());
}

// Live RetroAchievements rich-presence string — the rcheevos client
// recomputes this each second from the game's RAM (see Achievements.cpp
// UpdateRichPresence). On Android the AchievementsPanel polls this
// alongside the achievements JSON; rcheevos also auto-pings the RA
// server with the same string so the user's RA profile shows it. Returns
// empty string when no client / no game / RP not yet computed.
extern "C"
JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getRichPresence(JNIEnv *env, jclass clazz) {
    if (!Achievements::HasRichPresence())
        return env->NewStringUTF("");
    return env->NewStringUTF(Achievements::GetRichPresenceString().c_str());
}

// RetroAchievements password login. Synchronous — Achievements::Login waits
// for the HTTP request internally. Returns null on success, otherwise a
// human-readable error string (rcheevos message or "Failed to create
// client" / "Failed to create login request"). Callers should dispatch
// off the Main thread; the request is HTTP and may take a few seconds.
extern "C"
JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_loginAchievements(JNIEnv *env, jclass clazz,
                                                       jstring p_user, jstring p_pass) {
    const std::string user = GetJavaString(env, p_user);
    const std::string pass = GetJavaString(env, p_pass);
    Error error;
    const bool ok = Achievements::Login(user.c_str(), pass.c_str(), &error);
    if (!ok)
    {
        const std::string msg = error.GetDescription();
        return env->NewStringUTF(msg.empty() ? "Login failed." : msg.c_str());
    }

    // Login wrote Username to BASE (in-memory, lost on restart) and Token
    // to SECRETS (file-backed via INISettingsInterface — persists). Mirror
    // Username INTO secrets.ini too so the next app launch can re-push it
    // to BASE before Achievements::Initialize runs. Without this the user
    // re-logs in every launch even though the token survives.
    if (s_secrets_settings_interface)
    {
        s_secrets_settings_interface->SetStringValue("Achievements", "Username", user.c_str());
        s_secrets_settings_interface->Save();
    }

    // Achievements::Initialize is gated on EmuConfig.Achievements.Enabled —
    // a returning user with the old default-off config might still have it
    // off. Push Enabled=true and ApplySettings so UpdateSettings detects
    // the change and runs Initialize for any current/future VM. Initialize
    // reads the just-persisted Token and re-logs in on the persistent
    // s_client, then BeginLoadGame loads the running game's achievement
    // set.
    Host::SetBaseBoolSettingValue("Achievements", "Enabled", true);
    if (VMManager::HasValidVM())
        VMManager::ApplySettings();
    return nullptr;
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_logoutAchievements(JNIEnv *env, jclass clazz) {
    Achievements::Logout();
    // Achievements::Logout clears Token from SECRETS but leaves the
    // Username we co-stored there. Drop it too so a fresh launch doesn't
    // re-mirror a stale username back to BASE.
    if (s_secrets_settings_interface)
    {
        s_secrets_settings_interface->DeleteValue("Achievements", "Username");
        s_secrets_settings_interface->Save();
    }
}

// Enable / disable RetroAchievements hardcore mode. Writes the BASE
// EmuConfig.Achievements.HardcoreMode flag and applies it via
// VMManager::ApplySettings — the settings-diff path in
// Achievements::UpdateSettings() then either calls ResetHardcoreMode()
// (turn-on requires a fresh boot per upstream's design) or
// DisableHardcoreMode() (turn-off applies live). The Kotlin side is
// expected to gate "Enable HC?" on a "this will reset the running game"
// confirm before calling this with `true`.
extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setHardcoreMode(JNIEnv *env, jclass clazz, jboolean enabled) {
    Host::SetBaseBoolSettingValue("Achievements", "HardcoreMode", enabled == JNI_TRUE);
    if (VMManager::HasValidVM())
        VMManager::ApplySettings();
}

// Returns the live hardcore-mode flag (rcheevos s_hardcore_mode), not the
// persisted EmuConfig setting — they can transiently differ while a
// hardcore-enable is waiting for the next boot.
extern "C"
JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_isHardcoreMode(JNIEnv *env, jclass clazz) {
    return Achievements::IsHardcoreModeActive() ? JNI_TRUE : JNI_FALSE;
}

// Live HW/SW state from the GS thread's POV. The in-game overlay's renderer
// pill mirrors this on every poll so an emucore-driven swap (e.g. SoftwareRendererFMVHack
// flipping to SW during an FMV) doesn't desync the UI from the actual state.
extern "C"
JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_isHardwareRenderer(JNIEnv *env, jclass clazz) {
    return GSIsHardwareRenderer() ? JNI_TRUE : JNI_FALSE;
}

// Custom Vulkan driver pin. Called from Main.applyRendererPrefs BEFORE the
// VM starts so the first MTGS::Open (which triggers Vulkan::LoadVulkanLibrary)
// picks up the custom driver. Empty strings revert to the system loader.
// See Vulkan::SetCustomDriverPath in VKLoader.cpp for the splice.
extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setCustomVulkanDriver(
    JNIEnv* env, jclass clazz,
    jstring driverDir, jstring driverName,
    jstring redirectDir, jstring hookLibDir) {
    const std::string dir   = GetJavaString(env, driverDir);
    const std::string name  = GetJavaString(env, driverName);
    const std::string redir = GetJavaString(env, redirectDir);
    const std::string hook  = GetJavaString(env, hookLibDir);
    Vulkan::SetCustomDriverPath(
        dir.c_str(), name.c_str(), redir.c_str(), hook.c_str());
}

extern "C"
JNIEXPORT jfloat JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getFPS(JNIEnv *env, jclass clazz) {
    return (jfloat)PerformanceMetrics::GetFPS();
}

extern "C"
JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getPauseGameTitle(JNIEnv *env, jclass clazz) {
    std::string ret = VMManager::GetTitle(true);
    return env->NewStringUTF(ret.c_str());
}

extern "C"
JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getPauseGameSerial(JNIEnv *env, jclass clazz) {
    std::string ret = StringUtil::StdStringFromFormat("%s (%08X)", VMManager::GetDiscSerial().c_str(), VMManager::GetDiscCRC());
    return env->NewStringUTF(ret.c_str());
}


extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setPadVibration(JNIEnv *env, jclass clazz,
                                                     jboolean p_isOnOff) {
}


extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setPadButton(JNIEnv *env, jclass clazz,
                                                  jint p_key, jint p_range, jboolean p_keyPressed) {
    PadDualshock2::Inputs _key;
    switch (p_key) {
        case 19: _key = PadDualshock2::Inputs::PAD_UP; break;
        case 22: _key = PadDualshock2::Inputs::PAD_RIGHT; break;
        case 20: _key = PadDualshock2::Inputs::PAD_DOWN; break;
        case 21: _key = PadDualshock2::Inputs::PAD_LEFT; break;
        case 100: _key = PadDualshock2::Inputs::PAD_TRIANGLE; break;
        case 97: _key = PadDualshock2::Inputs::PAD_CIRCLE; break;
        case 96: _key = PadDualshock2::Inputs::PAD_CROSS; break;
        case 99: _key = PadDualshock2::Inputs::PAD_SQUARE; break;
        case 109: _key = PadDualshock2::Inputs::PAD_SELECT; break;
        case 108: _key = PadDualshock2::Inputs::PAD_START; break;
        case 102: _key = PadDualshock2::Inputs::PAD_L1; break;
        case 104: _key = PadDualshock2::Inputs::PAD_L2; break;
        case 103: _key = PadDualshock2::Inputs::PAD_R1; break;
        case 105: _key = PadDualshock2::Inputs::PAD_R2; break;
        case 106: _key = PadDualshock2::Inputs::PAD_L3; break;
        case 107: _key = PadDualshock2::Inputs::PAD_R3; break;
        case 110: _key = PadDualshock2::Inputs::PAD_L_UP; break;
        case 111: _key = PadDualshock2::Inputs::PAD_L_RIGHT; break;
        case 112: _key = PadDualshock2::Inputs::PAD_L_DOWN; break;
        case 113: _key = PadDualshock2::Inputs::PAD_L_LEFT; break;
        case 120: _key = PadDualshock2::Inputs::PAD_R_UP; break;
        case 121: _key = PadDualshock2::Inputs::PAD_R_RIGHT; break;
        case 122: _key = PadDualshock2::Inputs::PAD_R_DOWN; break;
        case 123: _key = PadDualshock2::Inputs::PAD_R_LEFT; break;
        default: _key = PadDualshock2::Inputs::PAD_CROSS ; break;
    }

    // Analog axis inputs (keycodes 110-123) carry a 0-32767 magnitude in p_range.
    // Digital buttons always use 1.0/0.0.
    const float state = p_keyPressed
        ? ((p_range > 0) ? (p_range / 32767.0f) : 1.0f)
        : 0.0f;
    Pad::SetControllerState(0, static_cast<u32>(_key), state);
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_resetKeyStatus(JNIEnv *env, jclass clazz) {
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setEnableCheats(JNIEnv *env, jclass clazz,
                                                     jboolean p_isonoff) {
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setAspectRatio(JNIEnv *env, jclass clazz,
                                                    jint p_type) {
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_speedhackLimitermode(JNIEnv *env, jclass clazz,
                                                          jint p_value) {
    // Enum values match LimiterModeType (Config.h:267):
    //   0 Nominal, 1 Turbo, 2 Slomo, 3 Unlimited.
    // Called from the in-game overlay's Frame Limit toggle (0 vs 3).
    // SetLimiterMode is a small atomic update + UpdateTargetSpeed —
    // safe to call from the JNI thread without RunOnCPUThread (the
    // Android port doesn't have one wired anyway). No-op when no VM
    // is running so we don't poke stale state.
    if (!VMManager::HasValidVM())
        return;
    LimiterModeType mode;
    switch (p_value) {
        case 0: mode = LimiterModeType::Nominal; break;
        case 1: mode = LimiterModeType::Turbo; break;
        case 2: mode = LimiterModeType::Slomo; break;
        case 3: mode = LimiterModeType::Unlimited; break;
        default: return;
    }
    VMManager::SetLimiterMode(mode);
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_speedhackEecyclerate(JNIEnv *env, jclass clazz,
                                                          jint p_value) {
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_speedhackEecycleskip(JNIEnv *env, jclass clazz,
                                                          jint p_value) {
}

// Generic setting writer — mirror of pcsx2-qt's settings save path.
// Writes flow into s_settings_interface (the MemorySettingsInterface
// installed in initialize); commitSettings flushes them through to the
// VM. Type comes as a string from Java to keep the JNI surface flat —
// only four primitives are supported (bool/int/float/string), enough
// for every EmuCore key the UI needs to push.
extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_setSetting(JNIEnv *env, jclass clazz,
                                                 jstring p_section, jstring p_key,
                                                 jstring p_type, jstring p_value) {
    const std::string section = GetJavaString(env, p_section);
    const std::string key     = GetJavaString(env, p_key);
    const std::string type    = GetJavaString(env, p_type);
    const std::string value   = GetJavaString(env, p_value);

    // Project builds with -fno-exceptions, so std::stoi / std::stof can't
    // be wrapped in try-catch. Use StringUtil::FromChars (the same parser
    // MemorySettingsInterface uses internally for GetIntValue/GetFloatValue
    // — guarantees parse-symmetry with whatever we write here).
    if (type == "bool")
    {
        const bool bval = (value == "true" || value == "1");
        Host::SetBaseBoolSettingValue(section.c_str(), key.c_str(), bval);
    }
    else if (type == "int")
    {
        if (auto parsed = StringUtil::FromChars<s32>(value, 10); parsed.has_value())
            Host::SetBaseIntSettingValue(section.c_str(), key.c_str(), parsed.value());
    }
    else if (type == "float")
    {
        if (auto parsed = StringUtil::FromChars<float>(value); parsed.has_value())
            Host::SetBaseFloatSettingValue(section.c_str(), key.c_str(), parsed.value());
    }
    else if (type == "string")
    {
        Host::SetBaseStringSettingValue(section.c_str(), key.c_str(), value.c_str());
    }
    else
    {
        Console.Warning("setSetting: unknown type '%s' for %s/%s", type.c_str(),
                        section.c_str(), key.c_str());
    }
}

// Push queued setSetting writes into the running VM. Idempotent — safe
// to call multiple times. Logs the resolved EmuCore.Speedhacks state
// for plumbing-verification (one-line check from logcat).
extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_commitSettings(JNIEnv *env, jclass clazz) {
    if (VMManager::HasValidVM())
        VMManager::ApplySettings();
    if (MTGS::IsOpen())
        MTGS::ApplySettings();

    // Plumbing roundtrip verifier — once the UI starts pushing real
    // settings, watch logcat for these to confirm the write landed.
    // Unary `+` promotes each bit-field to a plain int, since C++ doesn't
    // allow forwarding references (T&&) to bind to bit-fields.
    Console.WriteLnFmt(
        "Settings commit: vuThread={} EECycleRate={} EECycleSkip={} "
        "vu1Instant={} fastCDVD={} vuFlagHack={}",
        +EmuConfig.Speedhacks.vuThread,
        +EmuConfig.Speedhacks.EECycleRate,
        +EmuConfig.Speedhacks.EECycleSkip,
        +EmuConfig.Speedhacks.vu1Instant,
        +EmuConfig.Speedhacks.fastCDVD,
        +EmuConfig.Speedhacks.vuFlagHack);
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_renderUpscalemultiplier(JNIEnv *env, jclass clazz,
                                                             jfloat p_value) {
    // VMManager::ApplySettings (called inside Initialize) resets EmuConfig
    // from the persistent SettingsInterface, so writing only to EmuConfig
    // pre-launch gets clobbered. Push to the BASE layer so LoadCoreSettings
    // picks it up. Also update EmuConfig directly + nudge MTGS so a live
    // VM picks up the change without a settings file save round-trip.
    Host::SetBaseFloatSettingValue("EmuCore/GS", "upscale_multiplier", p_value);
    EmuConfig.GS.UpscaleMultiplier = p_value;
    if (MTGS::IsOpen())
        MTGS::ApplySettings();
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_renderMipmap(JNIEnv *env, jclass clazz,
                                                  jint p_value) {
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_renderHalfpixeloffset(JNIEnv *env, jclass clazz,
                                                           jint p_value) {
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_renderPreloading(JNIEnv *env, jclass clazz,
                                                      jint p_value) {
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_renderSoftware(JNIEnv *env, jclass clazz) {
    // Don't go through MTGS::ApplySettings → GSUpdateConfig → GSreopen(true,true,SW)
    // here: GSreopen(recreate_device=true, SW) calls GetAPIForRenderer(SW), which
    // falls to the default branch and asks GSUtil::GetPreferredRenderer for an API.
    // On Android that resolves to OpenGL — so going SW after picking Vulkan in the
    // wizard would silently rebuild GSDeviceOGL and the SW renderer would present
    // via GL, not VK.
    //
    // SetSoftwareRendering preserves the existing GSDevice (VK stays VK, OGL stays
    // OGL) and only swaps the renderer to SW. The picked backend remains the host
    // display device.
    EmuConfig.GS.Renderer = GSRendererType::SW;
    if(MTGS::IsOpen()) {
        MTGS::SetSoftwareRendering(true, EmuConfig.GS.InterlaceMode, false);
    }
}

// Auto = let GSUtil::GetPreferredRenderer pick at runtime based on what
// the device supports (Vulkan when available, OpenGL otherwise, SW as
// last resort). Matches Pcsx2Config::DEFAULT_HW_RENDERER and is the
// fresh-install default — the in-game overlay's renderer cycle still
// allows explicit OPENGL/SW override on top.
extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_renderAuto(JNIEnv *env, jclass clazz) {
    Host::SetBaseIntSettingValue("EmuCore/GS", "Renderer",
        static_cast<int>(GSRendererType::Auto));
    EmuConfig.GS.Renderer = GSRendererType::Auto;
    if(MTGS::IsOpen()) {
        MTGS::ApplySettings();
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_renderOpenGL(JNIEnv *env, jclass clazz) {
    Host::SetBaseIntSettingValue("EmuCore/GS", "Renderer",
        static_cast<int>(GSRendererType::OGL));
    EmuConfig.GS.Renderer = GSRendererType::OGL;
    if(MTGS::IsOpen()) {
        // In-game pill SW→HW: keep the existing OGL device, swap renderer to HW.
        // ApplySettings would do a full teardown which is fine here (same backend),
        // but SetSoftwareRendering is cheaper and matches the symmetric path used
        // by renderSoftware.
        MTGS::SetSoftwareRendering(false, EmuConfig.GS.InterlaceMode, false);
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_renderVulkan(JNIEnv *env, jclass clazz) {
    // Selecting "Vulkan" in the setup wizard means the host display device
    // is GSDeviceVK. We set Renderer=VK so MTGS::Open creates that device;
    // a subsequent renderSoftware() call then flips Renderer=SW but keeps
    // GSDeviceVK as the display, which is how the in-game HW/SW pill cycles
    // inside the user's chosen backend.
    //
    // VK HW had a known blending regression (BIOS pillars / SCEA text get
    // black boxes when AccBlendLevel = Full) — was masked here by a coerce
    // to SW. The coerce is removed because (a) the user explicitly picked
    // Vulkan, (b) without it SW couldn't get the VK display backend, and
    // (c) the AccBlendLevel default in the wizard is Full and the in-game
    // overlay has the toggle if the user hits the regression.
    Host::SetBaseIntSettingValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::VK));
    EmuConfig.GS.Renderer = GSRendererType::VK;
    if(MTGS::IsOpen()) {
        // In-game pill SW→HW with Vulkan backend: keep the existing VK device,
        // swap renderer to HW.
        MTGS::SetSoftwareRendering(false, EmuConfig.GS.InterlaceMode, false);
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_onNativeSurfaceCreated(JNIEnv *env, jclass clazz) {
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_onNativeSurfaceChanged(JNIEnv *env, jclass clazz,
                                                            jobject p_surface, jint p_width, jint p_height) {
    if(s_window) {
        ANativeWindow_release(s_window);
        s_window = nullptr;
    }

    if(p_surface != nullptr) {
        s_window = ANativeWindow_fromSurface(env, p_surface);
    }

    if(p_width > 0 && p_height > 0) {
        s_window_width = p_width;
        s_window_height = p_height;
        if(MTGS::IsOpen()) {
            MTGS::UpdateDisplayWindow();
        }
    }
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_onNativeSurfaceDestroyed(JNIEnv *env, jclass clazz) {
    if(s_window) {
        ANativeWindow_release(s_window);
        s_window = nullptr;
    }
}


std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
    float _fScale = 1.0;
    if (s_window_width > 0 && s_window_height > 0) {
        int _nSize = s_window_width;
        if (s_window_width <= s_window_height) {
            _nSize = s_window_height;
        }
        _fScale = (float)_nSize / 800.0f;
    }
    ////
    WindowInfo _windowInfo;
    memset(&_windowInfo, 0, sizeof(_windowInfo));
    _windowInfo.type = WindowInfo::Type::Android;
    _windowInfo.surface_width = s_window_width;
    _windowInfo.surface_height = s_window_height;
    _windowInfo.surface_scale = _fScale;
    _windowInfo.window_handle = s_window;

    return _windowInfo;
}

void Host::ReleaseRenderWindow() {

}

static s32 s_loop_count = 1;

// Owned by the GS thread.
static u32 s_dump_frame_number = 0;
static u32 s_loop_number = s_loop_count;
static double s_last_internal_draws = 0;
static double s_last_draws = 0;
static double s_last_render_passes = 0;
static double s_last_barriers = 0;
static double s_last_copies = 0;
static double s_last_uploads = 0;
static double s_last_readbacks = 0;
static u64 s_total_internal_draws = 0;
static u64 s_total_draws = 0;
static u64 s_total_render_passes = 0;
static u64 s_total_barriers = 0;
static u64 s_total_copies = 0;
static u64 s_total_uploads = 0;
static u64 s_total_readbacks = 0;
static u32 s_total_frames = 0;
static u32 s_total_drawn_frames = 0;

void Host::BeginPresentFrame() {
    if (GSIsHardwareRenderer())
    {
        const u32 last_draws = s_total_internal_draws;
        const u32 last_uploads = s_total_uploads;

        static constexpr auto update_stat = [](GSPerfMon::counter_t counter, u64& dst, double& last) {
            // perfmon resets every 30 frames to zero
            const double val = g_perfmon.GetCounter(counter);
            dst += static_cast<u64>((val < last) ? val : (val - last));
            last = val;
        };

        update_stat(GSPerfMon::Draw, s_total_internal_draws, s_last_internal_draws);
        update_stat(GSPerfMon::DrawCalls, s_total_draws, s_last_draws);
        update_stat(GSPerfMon::RenderPasses, s_total_render_passes, s_last_render_passes);
        update_stat(GSPerfMon::Barriers, s_total_barriers, s_last_barriers);
        update_stat(GSPerfMon::TextureCopies, s_total_copies, s_last_copies);
        update_stat(GSPerfMon::TextureUploads, s_total_uploads, s_last_uploads);
        update_stat(GSPerfMon::Readbacks, s_total_readbacks, s_last_readbacks);

        const bool idle_frame = s_total_frames && (last_draws == s_total_internal_draws && last_uploads == s_total_uploads);

        if (!idle_frame)
            s_total_drawn_frames++;

        s_total_frames++;

        std::atomic_thread_fence(std::memory_order_release);
    }
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
                         const std::string& disc_serial, u32 disc_crc, u32 current_crc) {
}

void Host::PumpMessagesOnCPUThread() {
}

int FileSystem::OpenFDFileContent(const char* filename)
{
    auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    if(env == nullptr) {
        return -1;
    }
    jclass NativeApp = env->FindClass("kr/co/iefriends/pcsx2/NativeApp");
    jmethodID openContentUri = env->GetStaticMethodID(NativeApp, "openContentUri", "(Ljava/lang/String;)I");

    jstring j_filename = env->NewStringUTF(filename);
    int fd = env->CallStaticIntMethod(NativeApp, openContentUri, j_filename);
    return fd;
}

void ReportTestResults(const char* label, int passed, int total)
{
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (!env) return;
    jclass clazz = env->FindClass("kr/co/iefriends/pcsx2/NativeApp");
    if (!clazz) return;
    jmethodID mid = env->GetStaticMethodID(clazz, "onTestResults", "(Ljava/lang/String;II)V");
    if (!mid) { env->DeleteLocalRef(clazz); return; }
    jstring jlabel = env->NewStringUTF(label);
    env->CallStaticVoidMethod(clazz, mid, jlabel, (jint)passed, (jint)total);
    env->DeleteLocalRef(jlabel);
    env->DeleteLocalRef(clazz);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_runVMThread(JNIEnv *env, jclass clazz,
                                                 jstring p_szpath) {
    std::string _szPath = GetJavaString(env, p_szpath);
    Console.WriteLn("ARMSX2-START");
    /////////////////////////////

    s_execute_exit = false;

//    const char* error;
//    if (!VMManager::PerformEarlyHardwareChecks(&error)) {
//        return false;
//    }

    // fast_boot : (false:bios->game, true:game)
    VMBootParameters boot_params;
    boot_params.filename = _szPath;
    boot_params.fast_boot = false;
    Console.Error("Loading %s", _szPath.c_str());
    if (!VMManager::Internal::CPUThreadInitialize()) {
        VMManager::Internal::CPUThreadShutdown();
    }

    // Wait for Android surface before opening GS
    while (!s_window)
        usleep(10000);

    VMManager::ApplySettings();
    GSDumpReplayer::SetIsDumpRunner(false);

    if (VMManager::Initialize(boot_params, nullptr) == VMBootResult::StartupSuccess)
    {
        Console.Error("VM INIT");
        // Apply the persisted frame-limit preference now that the VM is up.
        // The overlay's Frame Limiter toggle stores into the base layer via
        // setSetting("EmuCore/GS","FrameLimitEnable") + speedhackLimitermode
        // for live-apply; the live-apply early-returns when no VM exists, so
        // on a cold start the saved preference would otherwise be ignored
        // until the user toggled it. Default is `true` (Nominal) to match
        // VMManager::SetDefaultSettings's behaviour.
        const bool frame_limit_on = Host::GetBaseBoolSettingValue(
            "EmuCore/GS", "FrameLimitEnable", true);
        VMManager::SetLimiterMode(frame_limit_on ? LimiterModeType::Nominal
                                                 : LimiterModeType::Unlimited);
        VMState _vmState = VMState::Running;
        VMManager::SetState(_vmState);
        ////
        while (true) {
            _vmState = VMManager::GetState();
            if (_vmState == VMState::Stopping || _vmState == VMState::Shutdown) {
                break;
            } else if (_vmState == VMState::Running) {
                s_execute_exit = false;
                VMManager::Execute();
                s_execute_exit = true;
            } else {
                usleep(250000);
            }
        }
        ////
        VMManager::Shutdown(false);
    }
    ////
    VMManager::Internal::CPUThreadShutdown();

    return true;
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_pause(JNIEnv *env, jclass clazz) {
    std::thread([] {
        VMManager::SetPaused(true);
    }).detach();
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_resume(JNIEnv *env, jclass clazz) {
    std::thread([] {
        VMManager::SetPaused(false);
    }).detach();
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_flushShaderCache(JNIEnv *env, jclass clazz) {
    // Persist the Vulkan pipeline cache so cold restarts don't re-compile every
    // pipeline. Hooked from onPause so the typical background-then-swipe-kill
    // sequence on Android still writes the cache. The destructor in
    // VKShaderCache also flushes, but we can't rely on onDestroy running before
    // Android reaps the process. No-op for the OpenGL backend (GL backend
    // manages its own cache via GLShaderCache; this is Vulkan-specific).
    if (g_vulkan_shader_cache)
        g_vulkan_shader_cache->FlushPipelineCache();
}

extern "C"
JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_shutdown(JNIEnv *env, jclass clazz) {
    // Only signal Stopping when there's actually a VM to stop. Calling
    // SetState(Stopping) with no active VM leaves s_state stuck at Stopping,
    // which then makes the next VMManager::Initialize fail (it requires
    // s_state == Shutdown). Symptom was a "hang" on first card-tap launch.
    if (VMManager::HasValidVM())
        VMManager::SetState(VMState::Stopping);
}


extern "C"
JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_saveStateToSlot(JNIEnv *env, jclass clazz, jint p_slot) {
    // Previous body was a TODO stub spinning on s_execute_exit with the
    // actual SaveStateToSlot call commented out — that's why save was
    // doing nothing. Now we just call straight through.
    //
    // Caller (Kotlin SaveStatePicker) dispatches this on a background
    // thread and pauses the VM beforehand (overlay path), so blocking
    // here is fine. zip_on_thread=false → the zip is finalized before
    // we return, so the slot's Screenshot.png is on disk by the time
    // the picker re-reads slot state. The screenshot is captured by
    // VMManager::SaveStateToSlot from the GS framebuffer automatically
    // — no separate GSQueueSnapshot needed.
    if (!VMManager::HasValidVM())
        return false;
    if (VMManager::GetDiscCRC() == 0)
        return false;
    // SaveStateToSlot calls error_callback on failure paths (memcard busy,
    // bad path) — passing a null std::function would std::bad_function_call.
    // No-op lambda swallows errors silently for now; proper UI surfacing
    // can be wired later if needed.
    VMManager::SaveStateToSlot(p_slot, /*zip_on_thread=*/false,
        [](const std::string&) {});
    return true;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_loadStateFromSlot(JNIEnv *env, jclass clazz, jint p_slot) {
    // Previous body spun for 5 seconds waiting for s_execute_exit
    // before ever calling LoadStateFromSlot — typically the flag
    // never flipped within the window so load was a no-op. Direct
    // call now (caller pauses + dispatches off-thread).
    if (!VMManager::HasValidVM())
        return false;
    const u32 _crc = VMManager::GetDiscCRC();
    if (_crc == 0)
        return false;
    if (!VMManager::HasSaveStateInSlot(VMManager::GetDiscSerial().c_str(), _crc, p_slot))
        return false;
    return VMManager::LoadStateFromSlot(p_slot);
}

extern "C"
JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getGamePathSlot(JNIEnv *env, jclass clazz, jint p_slot) {
    std::string _filename = VMManager::GetSaveStateFileName(VMManager::GetDiscSerial().c_str(), VMManager::GetDiscCRC(), p_slot);
    if(!_filename.empty()) {
        return env->NewStringUTF(_filename.c_str());
    }
    return nullptr;
}

extern "C"
JNIEXPORT jbyteArray JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getImageSlot(JNIEnv *env, jclass clazz, jint p_slot) {
    jbyteArray retArr = nullptr;

    std::string _filename = VMManager::GetSaveStateFileName(VMManager::GetDiscSerial().c_str(), VMManager::GetDiscCRC(), p_slot);
    if(!_filename.empty())
    {
        zip_error_t ze = {};
        auto zf = zip_open_managed(_filename.c_str(), ZIP_RDONLY, &ze);
        if (zf) {
            auto zff = zip_fopen_managed(zf.get(), "Screenshot.png", 0);
            if(zff) {
                std::optional<std::vector<u8>> optdata(ReadBinaryFileInZip(zff.get()));
                if (optdata.has_value()) {
                    std::vector<u8> vec = std::move(optdata.value());
                    ////
                    auto length = static_cast<jsize>(vec.size());
                    retArr = env->NewByteArray(length);
                    if (retArr != nullptr) {
                        env->SetByteArrayRegion(retArr, 0, length,
                                                reinterpret_cast<const jbyte *>(vec.data()));
                    }
                }
            }
        }
    }

    return retArr;
}

// =====================  Autosave-on-exit slot  =====================
// Backed by VMManager::SAVESTATE_SLOT_AUTOSAVE (s32 sentinel = -2),
// stored as `{serial} (CRC).autosave.p2s`. Lets "Save State And Exit"
// avoid clobbering user slot 0; the load picker surfaces the autosave
// tile only when hasAutosaveState() returns true.

extern "C"
JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_saveAutosaveState(JNIEnv *env, jclass clazz) {
    if (!VMManager::HasValidVM())
        return false;
    if (VMManager::GetDiscCRC() == 0)
        return false;
    VMManager::SaveStateToSlot(VMManager::SAVESTATE_SLOT_AUTOSAVE, /*zip_on_thread=*/false,
        [](const std::string&) {});
    return true;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_loadAutosaveState(JNIEnv *env, jclass clazz) {
    if (!VMManager::HasValidVM())
        return false;
    const u32 _crc = VMManager::GetDiscCRC();
    if (_crc == 0)
        return false;
    if (!VMManager::HasSaveStateInSlot(VMManager::GetDiscSerial().c_str(), _crc,
                                       VMManager::SAVESTATE_SLOT_AUTOSAVE))
        return false;
    return VMManager::LoadStateFromSlot(VMManager::SAVESTATE_SLOT_AUTOSAVE);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_hasAutosaveState(JNIEnv *env, jclass clazz) {
    if (!VMManager::HasValidVM())
        return false;
    const u32 _crc = VMManager::GetDiscCRC();
    if (_crc == 0)
        return false;
    return VMManager::HasSaveStateInSlot(VMManager::GetDiscSerial().c_str(), _crc,
                                         VMManager::SAVESTATE_SLOT_AUTOSAVE);
}

extern "C"
JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getAutosaveGamePath(JNIEnv *env, jclass clazz) {
    std::string _filename = VMManager::GetSaveStateFileName(VMManager::GetDiscSerial().c_str(),
                                                            VMManager::GetDiscCRC(),
                                                            VMManager::SAVESTATE_SLOT_AUTOSAVE);
    if (!_filename.empty())
        return env->NewStringUTF(_filename.c_str());
    return nullptr;
}

extern "C"
JNIEXPORT jbyteArray JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getAutosaveImage(JNIEnv *env, jclass clazz) {
    jbyteArray retArr = nullptr;

    std::string _filename = VMManager::GetSaveStateFileName(VMManager::GetDiscSerial().c_str(),
                                                            VMManager::GetDiscCRC(),
                                                            VMManager::SAVESTATE_SLOT_AUTOSAVE);
    if (!_filename.empty())
    {
        zip_error_t ze = {};
        auto zf = zip_open_managed(_filename.c_str(), ZIP_RDONLY, &ze);
        if (zf) {
            auto zff = zip_fopen_managed(zf.get(), "Screenshot.png", 0);
            if (zff) {
                std::optional<std::vector<u8>> optdata(ReadBinaryFileInZip(zff.get()));
                if (optdata.has_value()) {
                    std::vector<u8> vec = std::move(optdata.value());
                    auto length = static_cast<jsize>(vec.size());
                    retArr = env->NewByteArray(length);
                    if (retArr != nullptr) {
                        env->SetByteArrayRegion(retArr, 0, length,
                                                reinterpret_cast<const jbyte *>(vec.data()));
                    }
                }
            }
        }
    }

    return retArr;
}


void Host::CommitBaseSettingChanges()
{
    // nothing to save, we're all in memory
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
    // not running any UI, so no settings requests will come in
    return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
    // nothing
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
    return nullptr;
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
    if (!title.empty() && !message.empty())
        ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
    else if (!message.empty())
        ERROR_LOG("ReportErrorAsync: {}", message);
}

//TODO
/*bool Host::ConfirmMessage(const std::string_view title, const std::string_view message)
{
    if (!title.empty() && !message.empty())
        ERROR_LOG("ConfirmMessage: {}: {}", title, message);
    else if (!message.empty())
        ERROR_LOG("ConfirmMessage: {}", message);

    return true;
}*/

void Host::OpenURL(const std::string_view url)
{
    // noop
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
    return false;
}

void Host::BeginTextInput()
{
    // noop
}

void Host::EndTextInput()
{
    // noop
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
    return std::nullopt;
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnPerformanceMetricsUpdated()
{
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
    pxFailRel("Not implemented");
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
    return false;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
    VMManager::SetState(VMState::Stopping);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
    // noop
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
    // noop
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
    // noop
}

void Host::OnAchievementsRefreshed()
{
    // noop
}

void Host::OnCoverDownloaderOpenRequested()
{
    // noop
}

void Host::OnCreateMemoryCardOpenRequested()
{
    // noop
}

bool Host::ShouldPreferHostFileSelector()
{
    return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
                                     FileSelectorFilters filters, std::string_view initial_directory)
{
    callback(std::string());
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
    return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
    return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
    return nullptr;
}

s32 Host::Internal::GetTranslatedStringImpl(
        const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
    if (msg.size() > tbuf_space)
        return -1;
    else if (msg.empty())
        return 0;

    std::memcpy(tbuf, msg.data(), msg.size());
    return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
    TinyString count_str = TinyString::from_format("{}", count);

    std::string ret(msg);
    for (;;)
    {
        std::string::size_type pos = ret.find("%n");
        if (pos == std::string::npos)
            break;

        ret.replace(pos, pos + 2, count_str.view());
    }

    return ret;
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
}

bool Host::LocaleCircleConfirm()
{
    return false;
}

bool Host::InNoGUIMode()
{
    return false;
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
    return lhs.compare(rhs);
}

// OSD toggle helpers — apply immediately via EmuConfig.GS then push to MTGS
static void applyOsdSetting()
{
    if (MTGS::IsOpen())
        MTGS::ApplySettings();
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_osdShowCPU(JNIEnv*, jclass, jboolean enabled) {
    EmuConfig.GS.OsdShowCPU = enabled;
    applyOsdSetting();
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_osdShowGPU(JNIEnv*, jclass, jboolean enabled) {
    EmuConfig.GS.OsdShowGPU = enabled;
    applyOsdSetting();
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_osdShowFPS(JNIEnv*, jclass, jboolean enabled) {
    EmuConfig.GS.OsdShowFPS = enabled;
    applyOsdSetting();
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_osdShowVPS(JNIEnv*, jclass, jboolean enabled) {
    EmuConfig.GS.OsdShowVPS = enabled;
    applyOsdSetting();
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_osdShowSpeed(JNIEnv*, jclass, jboolean enabled) {
    EmuConfig.GS.OsdShowSpeed = enabled;
    applyOsdSetting();
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_osdShowResolution(JNIEnv*, jclass, jboolean enabled) {
    EmuConfig.GS.OsdShowResolution = enabled;
    applyOsdSetting();
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_osdShowGSStats(JNIEnv*, jclass, jboolean enabled) {
    EmuConfig.GS.OsdShowGSStats = enabled;
    applyOsdSetting();
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_osdShowVersion(JNIEnv*, jclass, jboolean enabled) {
    EmuConfig.GS.OsdShowVersion = enabled;
    applyOsdSetting();
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_osdShowFrameTimes(JNIEnv*, jclass, jboolean enabled) {
    EmuConfig.GS.OsdShowFrameTimes = enabled;
    applyOsdSetting();
}

// Master OSD toggle — flips every OSD bit we enable at first init in
// initialize() so the in-game overlay's OSD pill is a single switch.
// Writes BASE too so the state survives the next ApplySettings reload
// (live EmuConfig writes get clobbered otherwise — see the EmuConfig vs
// SettingsInterface gotcha note).
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_osdShowAll(JNIEnv*, jclass, jboolean enabled) {
    const bool e = enabled;
    EmuConfig.GS.OsdShowFPS = e;
    EmuConfig.GS.OsdShowSpeed = e;
    EmuConfig.GS.OsdShowResolution = e;
    EmuConfig.GS.OsdShowCPU = e;
    EmuConfig.GS.OsdShowGPU = e;
    EmuConfig.GS.OsdShowGSStats = e;
    EmuConfig.GS.OsdShowFrameTimes = e;
    EmuConfig.GS.OsdShowHardwareInfo = e;
    EmuConfig.GS.OsdShowVersion = e;
    EmuConfig.GS.OsdShowSettings = e;
    EmuConfig.GS.OsdShowInputs = e;

    Host::SetBaseBoolSettingValue("EmuCore/GS", "OsdShowFPS", e);
    Host::SetBaseBoolSettingValue("EmuCore/GS", "OsdShowSpeed", e);
    Host::SetBaseBoolSettingValue("EmuCore/GS", "OsdShowResolution", e);
    Host::SetBaseBoolSettingValue("EmuCore/GS", "OsdShowCPU", e);
    Host::SetBaseBoolSettingValue("EmuCore/GS", "OsdShowGPU", e);
    Host::SetBaseBoolSettingValue("EmuCore/GS", "OsdShowGSStats", e);
    Host::SetBaseBoolSettingValue("EmuCore/GS", "OsdShowFrameTimes", e);
    Host::SetBaseBoolSettingValue("EmuCore/GS", "OsdShowHardwareInfo", e);
    Host::SetBaseBoolSettingValue("EmuCore/GS", "OsdShowVersion", e);
    Host::SetBaseBoolSettingValue("EmuCore/GS", "OsdShowSettings", e);
    Host::SetBaseBoolSettingValue("EmuCore/GS", "OsdShowInputs", e);

    applyOsdSetting();
}

extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_runCodegenTests(JNIEnv*, jclass) { RunArmCodegenTests(); }
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_runPatchTests(JNIEnv*, jclass) { RunPatchTests(); }
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_runVuJitTests(JNIEnv*, jclass) { RunVuJitTests(); }
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_runEeJitTests(JNIEnv*, jclass) { RunEeJitTests(); }
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_runVifTests(JNIEnv*, jclass) { RunVifTests(); }
extern "C" JNIEXPORT void JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_runEeSeqTests(JNIEnv*, jclass) { RunEeSeqTests(); }

// ---------------------------------------------------------------------------
// PS2 disc serial probe via ISO9660 directory walk.
//
// Reads the Primary Volume Descriptor at LBA 16, walks the root directory
// to find SYSTEM.CNF, parses its `BOOT2 = cdrom0:\SCUS_XXX.XX;1` line, and
// returns the serial in normalized "AAAA-NNNNN" form. Used by the games-
// list scanner to attach real game IDs to entries (instead of guessing
// from filenames).
//
// Handles three on-disk sector layouts so .iso (DVD-style 2048-byte data
// sectors) and .bin (raw CD-format 2352-byte sectors, typical for older
// CD-ROM-format PS2 games) both work:
//
//   2048 / 0    plain ISO — every byte is data
//   2352 / 16   Mode 1 raw — 12 byte sync, 4 byte header, 2048 data, 288 ECC
//   2352 / 24   Mode 2 Form 1 raw — 16 sync+header, 8 subheader, 2048 data, 280 ECC
//
// We try them in order and the first one that finds a valid PVD wins.
// CHD / CSO / GZ remain unsupported (they need libchdr / libuu1 / libz);
// those formats fall back to filename parsing on the Kotlin side.
//
// fd ownership: consumed (closed via fclose on the wrapping FILE*),
// matching the IsBIOSFromFd contract.
// ---------------------------------------------------------------------------

namespace {
// Reader abstraction so the SYSTEM.CNF probe is independent of the
// underlying container (flat ISO/BIN via FILE*, CHD via libchdr). Each
// implementation knows how to fetch up to 2048 bytes of cooked data
// starting at a given LBA + intra-sector offset.
using DiscReader = std::function<bool(std::uint32_t lba, std::uint32_t skip, void* buf, std::size_t size)>;

// FILE*-backed reader for plain ISO (2048/0) and raw .bin (2352/16, 2352/24).
static DiscReader MakeFileReader(std::FILE* fp, std::uint32_t sectorSize, std::uint32_t dataOffset)
{
    return [fp, sectorSize, dataOffset](std::uint32_t lba, std::uint32_t skip, void* buf, std::size_t size) -> bool {
        const std::uint64_t off = static_cast<std::uint64_t>(lba) * sectorSize + dataOffset + skip;
        if (std::fseek(fp, static_cast<long>(off), SEEK_SET) != 0) return false;
        return std::fread(buf, 1, size, fp) == size;
    };
}

// CHD-backed reader. Pulls hunks via chd_read and indexes into them. The
// caller owns `chd` and the cached hunk buffer (so this lambda can be
// rebuilt cheaply across layout retries).
static DiscReader MakeChdReader(chd_file* chd, std::uint32_t hunkBytes,
    std::vector<std::uint8_t>& hunkBuf, std::int64_t& cachedHunk,
    std::uint32_t sectorSize, std::uint32_t dataOffset)
{
    return [chd, hunkBytes, &hunkBuf, &cachedHunk, sectorSize, dataOffset](
               std::uint32_t lba, std::uint32_t skip, void* buf, std::size_t size) -> bool {
        std::uint64_t byte_off = static_cast<std::uint64_t>(lba) * sectorSize + dataOffset + skip;
        auto* dst = static_cast<std::uint8_t*>(buf);
        std::size_t left = size;
        while (left > 0)
        {
            const std::int64_t hunk_id = static_cast<std::int64_t>(byte_off / hunkBytes);
            const std::uint32_t in_hunk = static_cast<std::uint32_t>(byte_off % hunkBytes);
            if (cachedHunk != hunk_id)
            {
                if (chd_read(chd, static_cast<int>(hunk_id), hunkBuf.data()) != CHDERR_NONE)
                    return false;
                cachedHunk = hunk_id;
            }
            const std::size_t avail = hunkBytes - in_hunk;
            const std::size_t want = std::min(left, avail);
            std::memcpy(dst, hunkBuf.data() + in_hunk, want);
            dst += want;
            byte_off += want;
            left -= want;
        }
        return true;
    };
}

// Read `size` bytes of disc data starting at the given LBA, walking
// sectors via the supplied reader callback. Reads can span multiple
// sectors.
static bool ReadDiscData(const DiscReader& read, std::uint32_t startLba, void* buf, std::size_t size)
{
    auto* dst = static_cast<std::uint8_t*>(buf);
    std::uint32_t lba = startLba;
    std::size_t left = size;
    std::size_t skip = 0; // first sector might be partially consumed by a prior call

    while (left > 0)
    {
        const std::size_t avail = 2048 - skip;
        const std::size_t want = std::min<std::size_t>(left, avail);
        if (!read(lba, static_cast<std::uint32_t>(skip), dst, want)) return false;
        dst += want;
        left -= want;
        lba++;
        skip = 0;
    }
    return true;
}

// Parse SYSTEM.CNF using the supplied reader. Empty return = "this layout
// didn't apply" — caller tries the next one.
static std::string ProbeSerialWithReader(const DiscReader& read)
{
    // PVD lives at LBA 16 in every ISO9660 image regardless of physical
    // sector layout.
    std::uint8_t pvd[2048];
    if (!ReadDiscData(read, 16, pvd, sizeof(pvd))) return {};
    if (pvd[0] != 1 || std::memcmp(&pvd[1], "CD001", 5) != 0) return {};

    std::uint32_t rootLba  = *reinterpret_cast<const std::uint32_t*>(&pvd[156 + 2]);
    std::uint32_t rootSize = *reinterpret_cast<const std::uint32_t*>(&pvd[156 + 10]);
    if (rootLba == 0 || rootSize == 0 || rootSize > 1024 * 1024) return {};

    std::vector<std::uint8_t> rootData(rootSize);
    if (!ReadDiscData(read, rootLba, rootData.data(), rootSize)) return {};

    std::uint32_t sysLba = 0;
    std::uint32_t sysSize = 0;
    {
        std::size_t off = 0;
        while (off + 33 < rootData.size())
        {
            std::uint8_t recLen = rootData[off];
            if (recLen == 0)
            {
                // Skip to next sector boundary in the (logical) directory.
                off = (off / 2048 + 1) * 2048;
                continue;
            }
            if (off + recLen > rootData.size()) break;

            std::uint8_t nameLen = rootData[off + 32];
            if (nameLen >= 10 && nameLen <= 12 && off + 33 + nameLen <= rootData.size())
            {
                const char* name = reinterpret_cast<const char*>(&rootData[off + 33]);
                if (strncasecmp(name, "SYSTEM.CNF", 10) == 0)
                {
                    sysLba  = *reinterpret_cast<const std::uint32_t*>(&rootData[off + 2]);
                    sysSize = *reinterpret_cast<const std::uint32_t*>(&rootData[off + 10]);
                    break;
                }
            }

            off += recLen;
        }
    }

    if (sysLba == 0 || sysSize == 0 || sysSize > 64 * 1024) return {};

    std::string contents(sysSize, '\0');
    if (!ReadDiscData(read, sysLba, contents.data(), sysSize)) return {};

    // SYSTEM.CNF format examples:
    //   PS2: BOOT2 = cdrom0:\SCUS_972.28;1
    //        VER = 1.00
    //        VMODE = NTSC
    //   PS1: BOOT = cdrom:\SLUS_007.13;1
    //        TCB = tcb=64
    //        EVENT = ev=51,b=2048,s=2048
    //        STACK = stack=801fff00
    // PS2 uses BOOT2, PS1 uses BOOT (no trailing 2). Same serial format
    // (4 letters + 3 digits + dot + 2 digits) so we share the regex.
    // Returned string is `<platform>:<serial>` so the Kotlin side knows
    // which cover repo to hit (xlenore/ps2-covers vs xlenore/psx-covers).
    const char* platform = "ps2";
    std::size_t bootPos = contents.find("BOOT2");
    if (bootPos == std::string::npos)
    {
        // Check for PS1 BOOT line. Must NOT match a substring of BOOT2 —
        // we already failed that. Rare edge: if a disc has both keys
        // (it shouldn't), BOOT2 wins, which is correct (PS2).
        bootPos = contents.find("BOOT");
        if (bootPos == std::string::npos)
            return {};
        platform = "ps1";
    }

    std::size_t lineEnd = contents.find_first_of("\r\n", bootPos);
    if (lineEnd == std::string::npos) lineEnd = contents.size();
    std::string bootLine = contents.substr(bootPos, lineEnd - bootPos);

    // icase: rare but some discs have lowercase SYSTEM.CNF. Result is
    // uppercased for cover-URL stability (xlenore/ps2-covers names files
    // SLUS-20001.jpg, all caps).
    std::regex serialRe(R"(([A-Z]{4})_([0-9]{3})\.([0-9]{2}))", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(bootLine, m, serialRe)) return {};

    std::string serial = m[1].str() + "-" + m[2].str() + m[3].str();
    std::transform(serial.begin(), serial.end(), serial.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return std::string(platform) + ":" + serial;
}

// Minimal core_file wrapper around an existing FILE*. libchdr only needs
// fsize/fread/fseek/fclose; we hand-roll them to avoid bringing in the
// emulator's heavyweight ChdCoreFileWrapper (which deals with parents and
// precaching that we don't need for a one-shot serial probe).
struct ChdProbeCoreFile
{
    core_file core{};
    std::FILE* fp = nullptr;
};

static std::uint64_t ChdProbe_FSize(core_file* f)
{
    auto* w = static_cast<ChdProbeCoreFile*>(f->argp);
    if (std::fseek(w->fp, 0, SEEK_END) != 0) return static_cast<std::uint64_t>(-1);
    long sz = std::ftell(w->fp);
    return sz < 0 ? static_cast<std::uint64_t>(-1) : static_cast<std::uint64_t>(sz);
}
static std::size_t ChdProbe_FRead(void* buf, std::size_t elm, std::size_t cnt, core_file* f)
{
    auto* w = static_cast<ChdProbeCoreFile*>(f->argp);
    return std::fread(buf, elm, cnt, w->fp);
}
static int ChdProbe_FSeek(core_file* f, std::int64_t offset, int whence)
{
    auto* w = static_cast<ChdProbeCoreFile*>(f->argp);
    return std::fseek(w->fp, static_cast<long>(offset), whence);
}
static int ChdProbe_FClose(core_file* f)
{
    auto* w = static_cast<ChdProbeCoreFile*>(f->argp);
    if (w->fp) std::fclose(w->fp);
    delete w;
    return 0;
}

// Detect "MComprHD" magic at the head of the file.
static bool IsChdMagic(std::FILE* fp)
{
    char hdr[8];
    if (std::fseek(fp, 0, SEEK_SET) != 0) return false;
    if (std::fread(hdr, 1, 8, fp) != 8) return false;
    std::fseek(fp, 0, SEEK_SET);
    return std::memcmp(hdr, "MComprHD", 8) == 0;
}

// Open a CHD on top of `fp` (ownership transferred on success — libchdr
// closes the file via the core_file's fclose). Returns null on any
// error and leaves `fp` open for the caller to close.
static chd_file* OpenChdFromFile(std::FILE* fp)
{
    auto* wrapper = new ChdProbeCoreFile();
    wrapper->fp = fp;
    wrapper->core.argp = wrapper;
    wrapper->core.fsize = ChdProbe_FSize;
    wrapper->core.fread = ChdProbe_FRead;
    wrapper->core.fseek = ChdProbe_FSeek;
    wrapper->core.fclose = ChdProbe_FClose;

    chd_file* chd = nullptr;
    chd_error err = chd_open_core_file(&wrapper->core, CHD_OPEN_READ, nullptr, &chd);
    if (err != CHDERR_NONE)
    {
        // libchdr always calls our core_file fclose on its failure paths,
        // which deletes the wrapper and closes fp. Don't double-free.
        return nullptr;
    }
    return chd;
}
} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getGameSerialFromFd(JNIEnv* env, jclass, jint fd)
{
    if (fd < 0)
        return nullptr;

    std::FILE* fp = ::fdopen(fd, "rb");
    if (!fp)
        return nullptr;

    std::string serial;

    if (IsChdMagic(fp))
    {
        // CHD path. libchdr takes ownership of fp via the core_file
        // wrapper — on success it'll be closed when chd_close runs; on
        // failure libchdr's internal cleanup also closes it. Either way
        // we must NOT fclose(fp) on this branch.
        chd_file* chd = OpenChdFromFile(fp);
        if (chd)
        {
            const chd_header* hdr = chd_get_header(chd);
            const std::uint32_t hunk_bytes = hdr->hunkbytes;
            const std::uint32_t unit_bytes = hdr->unitbytes;
            std::vector<std::uint8_t> hunk_buf(hunk_bytes);
            std::int64_t cached_hunk = -1;

            // CHD frames are `unit_bytes` long and the 2048 bytes of
            // cooked data sits somewhere inside each frame. chdman packs
            // PS2 DVD ISOs as 2448-byte units with a +24 offset (per the
            // pattern emucore's ChdFileReader/InputIsoFile uses). PS2 CDs
            // use 2448 + 16 (Mode 1) or +24 (Mode 2 Form 1). Some DVD
            // CHDs also come through as 2048-byte units with 0 offset.
            // Try every plausible offset in the actual unit size — these
            // are cheap (a couple of hunk reads) and the first match
            // wins.
            auto tryLayout = [&](std::uint32_t sectorSize, std::uint32_t dataOffset) {
                if (!serial.empty()) return;
                cached_hunk = -1; // forget the previous attempt's hunk
                auto reader = MakeChdReader(chd, hunk_bytes, hunk_buf, cached_hunk, sectorSize, dataOffset);
                serial = ProbeSerialWithReader(reader);
            };

            tryLayout(unit_bytes, 0);
            tryLayout(unit_bytes, 16);
            tryLayout(unit_bytes, 24);
            // Fallbacks for CHDs whose unit_bytes doesn't match the
            // canonical layouts (defensive — shouldn't normally hit).
            if (unit_bytes != 2048) tryLayout(2048, 0);
            if (unit_bytes != 2352)
            {
                tryLayout(2352, 16);
                tryLayout(2352, 24);
            }

            chd_close(chd); // closes the wrapped core_file (and thus fp)
        }
        else
        {
            // libchdr's cleanup path already closed fp via fclose on the
            // wrapper. Don't double-close.
        }
    }
    else
    {
        // Plain ISO / raw .bin path. .iso files are virtually always
        // 2048/0; .bin files are usually 2352/16 (Mode 1 raw); 2352/24
        // (Mode 2 Form 1) is rare on PS2 but cheap to try as a last
        // resort.
        if (serial.empty()) serial = ProbeSerialWithReader(MakeFileReader(fp, 2048, 0));
        if (serial.empty()) serial = ProbeSerialWithReader(MakeFileReader(fp, 2352, 16));
        if (serial.empty()) serial = ProbeSerialWithReader(MakeFileReader(fp, 2352, 24));
        std::fclose(fp);
    }

    if (serial.empty()) return nullptr;
    return env->NewStringUTF(serial.c_str());
}

// ---------------------------------------------------------------------------
// Compatibility lookup — given a normalized serial like "SLUS-20312", asks
// the bundled PCSX2 game database for the title's compatibility rating.
// Returns one of the GameDatabaseSchema::Compatibility enum values:
//   0 Unknown, 1 Nothing, 2 Intro, 3 Menu, 4 InGame, 5 Playable, 6 Perfect
// Mapping to the games-list star display happens on the Kotlin side.
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT jint JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getCompatibilityForSerial(JNIEnv* env, jclass, jstring jSerial)
{
    if (!jSerial) return 0;
    const std::string serial = GetJavaString(env, jSerial);
    if (serial.empty()) return 0;

    const GameDatabaseSchema::GameEntry* db_entry = GameDatabase::findGame(serial);
    if (!db_entry) return 0;
    return static_cast<jint>(db_entry->compat);
}

// ---------------------------------------------------------------------------
// BIOS info probe — invoked from the setup wizard while the user is picking
// a BIOS directory. Takes ownership of `fd` (the caller MUST have detached
// it from any ParcelFileDescriptor before passing it here). Returns a
// com.armsx2.BiosInfo on success, null if the file isn't a valid PS2 BIOS
// or any read step fails.
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT jobject JNICALL
Java_kr_co_iefriends_pcsx2_NativeApp_getBiosInfoFromFd(JNIEnv* env, jclass, jint fd)
{
    u32 version = 0;
    u32 region = 0;
    std::string description;
    std::string zone;

    // IsBIOSFromFd consumes the fd (closes via fclose on the wrapping FILE*).
    // If parsing fails it still closes the fd, so no leak path on either branch.
    if (!IsBIOSFromFd(static_cast<int>(fd), version, description, region, zone))
        return nullptr;

    jclass biosCls = env->FindClass("com/armsx2/BiosInfo");
    if (!biosCls)
        return nullptr;

    jmethodID ctor = env->GetMethodID(biosCls, "<init>",
        "(IILjava/lang/String;Ljava/lang/String;)V");
    if (!ctor)
    {
        env->DeleteLocalRef(biosCls);
        return nullptr;
    }

    jstring jdesc = env->NewStringUTF(description.c_str());
    jstring jzone = env->NewStringUTF(zone.c_str());
    jobject obj = env->NewObject(biosCls, ctor, static_cast<jint>(version),
        static_cast<jint>(region), jdesc, jzone);

    env->DeleteLocalRef(jdesc);
    env->DeleteLocalRef(jzone);
    env->DeleteLocalRef(biosCls);
    return obj;
}

void Native::vmSetPaused(bool paused) {
    if (!s_jvm || !s_NativeApp_class || !s_vmSetPaused_mid) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    const int status = s_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if (s_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    } else if (status != JNI_OK) {
        return;
    }

    env->CallStaticVoidMethod(s_NativeApp_class, s_vmSetPaused_mid, static_cast<jboolean>(paused));

    if (attached) s_jvm->DetachCurrentThread();
}