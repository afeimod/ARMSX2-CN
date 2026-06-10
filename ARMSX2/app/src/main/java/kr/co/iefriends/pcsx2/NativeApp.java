package kr.co.iefriends.pcsx2;

import android.content.ContentResolver;
import android.content.Context;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.view.Surface;

import com.armsx2.BiosInfo;
import com.armsx2.EmuState;
import com.armsx2.Main;
import com.armsx2.events.TestResult;

import java.io.File;
import java.lang.ref.WeakReference;

public class NativeApp {
	static {
		try {
			System.loadLibrary("emucore");
			hasNoNativeBinary = false;
			System.out.println("PCSX2_LOAD");
		} catch (UnsatisfiedLinkError e) {
			hasNoNativeBinary = true;
		}
	}

	public static boolean hasNoNativeBinary;


	protected static WeakReference<Context> mContext;
	public static Context getContext() {
		return mContext.get();
	}

	public static void initializeOnce(Context context) {
		mContext = new WeakReference<>(context);

		// Compute the app's externalFilesDir up front — it's the BIOS
		// folder (always app-owned + writable, where the setup wizard's
		// finishBiosStep deposits the BIOS file) and the fallback for
		// DataRoot when the user hasn't picked one.
		File externalFilesDir = context.getExternalFilesDir(null);
		if (externalFilesDir == null) {
			externalFilesDir = context.getDataDir();
		}
		String biosFolder = externalFilesDir.getAbsolutePath() + java.io.File.separator + "bios";

		// DataRoot: prefer the user-chosen system folder (SAF tree URI in
		// the `systemDir` pref, resolved to POSIX by Main.systemDirPosix).
		// Falls back to externalFilesDir when unset / unresolvable.
		String chosen = Main.Companion.systemDirPosix();
		String dataPath = (chosen != null) ? chosen : externalFilesDir.getAbsolutePath();

		initialize(dataPath, biosFolder, android.os.Build.VERSION.SDK_INT);
	}

	public static native void initialize(String path, String biosFolder, int apiVer);

	/**
	 * Push one EmuCore setting into the base settings layer. Mirrors
	 * pcsx2-qt's Settings save flow — Host::SetBase*SettingValue sticks
	 * in s_settings_interface. type ∈ {"bool","int","float","string"};
	 * value is the stringified payload (e.g. "true", "2", "2.5").
	 *
	 * Setting writes here are NOT live until commitSettings() is called.
	 * Batch the writes, then commit once so the VM applies them atomically.
	 */
	public static native void setSetting(String section, String key, String type, String value);

	/**
	 * Apply queued settings to a running VM (and the GS thread). Calls
	 * VMManager::ApplySettings + MTGS::ApplySettings. No-op when no VM
	 * is running — settings still take effect on the next runVMThread
	 * because they were pushed to the persistent base settings layer.
	 *
	 * Some settings need a VM restart (recompiler enables, EE cycle rate);
	 * the UI layer should flag those.
	 */
	public static native void commitSettings();
	public static native String getGameTitle(String path);
	public static native String getGameSerial();
	public static native float getFPS();

	/** Build version string from BuildVersion::GitRev — formatted as
	 *  "GitTagHi.GitTagMid.GitTagLo.ARMSX2Build-SNAPSHOT". Used by the
	 *  setup wizard + in-game overlay branding so the displayed version
	 *  tracks the C++ constants without a Kotlin-side hardcoded copy. */
	public static native String getBuildVersion();

	public static native String getPauseGameTitle();
	public static native String getPauseGameSerial();

	/** Snapshot the current game's achievements as JSON for the in-game
	 *  overlay's right-side panel. See Achievements::GetAchievementsAsJSON
	 *  in the C++ side for the schema. Returns the empty-state payload
	 *  (active=false, items=[]) when no game is loaded or not logged in. */
	public static native String getAchievementsJSON();

	/** Live RetroAchievements rich-presence string. Recomputed every
	 *  second on the native side from the game's RAM. Empty when no game,
	 *  no client, or RP not supported by the loaded set. */
	public static native String getRichPresence();

	/** RetroAchievements password login. Returns null on success or a
	 *  human-readable error string. Synchronous — runs the HTTP login
	 *  request to completion, may take a few seconds. Callers MUST
	 *  dispatch off the Main thread (Dispatchers.IO from Compose).
	 *  After success the next getAchievementsJSON poll will reflect
	 *  loggedIn=true; no separate callback wiring needed. */
	public static native String loginAchievements(String username, String password);

	/** RetroAchievements logout. Idempotent. */
	public static native void logoutAchievements();

	/** Toggle RetroAchievements hardcore mode. Writes
	 *  EmuConfig.Achievements.HardcoreMode and triggers ApplySettings —
	 *  enabling hardcore on a running VM resets it on the next frame
	 *  (per upstream's design). Save states / cheats / runahead are
	 *  blocked by Achievements.cpp gates while active. */
	public static native void setHardcoreMode(boolean enabled);

	/** True iff the rcheevos hardcore flag is currently set. The Kotlin
	 *  achievements panel polls this for the badge / button colour. */
	public static native boolean isHardcoreMode();

	/** True iff the GS is currently in a HW renderer (OGL/VK), false for
	 *  SW. Mirrors GSIsHardwareRenderer() from the GS thread. Polled by
	 *  the in-game overlay's renderer pill so emucore-driven swaps
	 *  (e.g. SoftwareRendererFMVHack) stay in sync with the UI. */
	public static native boolean isHardwareRenderer();

	/** Master OSD toggle — flips every OsdShow* bit we enable at first
	 *  init. Backs the in-game overlay's OSD pill. */
	public static native void osdShowAll(boolean enabled);

	/** Pin a custom Vulkan driver (e.g. Mesa Turnip) for the next VM
	 *  start. Must be called BEFORE Main.start() — the first MTGS::Open
	 *  triggers Vulkan::LoadVulkanLibrary which reads these paths. Pass
	 *  empty strings to revert to the system loader.
	 *
	 *  driverDir:    /data/.../files/drivers/&lt;id&gt;/ (trailing slash required)
	 *  driverName:   e.g. "libvulkan_freedreno.so"
	 *  redirectDir:  /data/.../files/drivers/&lt;id&gt;/cache/ — Turnip shader cache target
	 *  hookLibDir:   ApplicationInfo.nativeLibraryDir — where the adrenotools
	 *                hook .so's (main_hook etc.) were extracted. */
	public static native void setCustomVulkanDriver(
		String driverDir, String driverName,
		String redirectDir, String hookLibDir);

	public static native void setPadVibration(boolean isonoff);
	public static native void setPadButton(int index, int range, boolean iskeypressed);
	public static native void resetKeyStatus();

	public static native void setAspectRatio(int type);
	public static native void speedhackLimitermode(int value);
	public static native void speedhackEecyclerate(int value);
	public static native void speedhackEecycleskip(int value);

	public static native void renderUpscalemultiplier(float value);
	public static native void renderMipmap(int value);
	public static native void renderHalfpixeloffset(int value);
	public static native void renderSoftware();
	public static native void renderOpenGL();
	public static native void renderVulkan();
	public static native void renderAuto();
	public static native void renderPreloading(int value);

	public static native void onNativeSurfaceCreated();
	public static native void onNativeSurfaceChanged(Surface surface, int w, int h);
	public static native void onNativeSurfaceDestroyed();

	public static native boolean runVMThread(String path);
	public static native void pause();
	public static native void resume();
	public static native void shutdown();

	/** Persist the Vulkan pipeline cache to disk so cold restarts don't have
	 *  to recompile every TFX pipeline. No-op for OpenGL (its cache flushes
	 *  on its own). Called from Main.onPause so backgrounding the app saves
	 *  the cache before Android can reap the process. Safe to call when no
	 *  Vulkan device is active (becomes a no-op). */
	public static native void flushShaderCache();

	/** Runs ARM64 codegen tests and prints PASS/FAIL to logcat (tag: ARM64CodegenTest). */
	public static native void runCodegenTests();

	/** Runs Patch::ApplyPatches tests and prints PASS/FAIL to logcat (tag: PatchTests). */
	public static native void runPatchTests();

	/** Runs microVU JIT integer-instruction tests and prints PASS/FAIL to logcat (tag: VuJitTests). */
	public static native void runVuJitTests();

	/** Runs R5900 EE interpreter instruction tests and prints PASS/FAIL to logcat (tag: EeJitTests). */
	public static native void runEeJitTests();

	/** Runs VIF UNPACK C++ template tests and prints PASS/FAIL to logcat (tag: VifTests). */
	public static native void runVifTests();

	/** Runs EE multi-instruction sequence tests and prints PASS/FAIL to logcat (tag: EeSeqTests). */
	public static native void runEeSeqTests();

	/** Called from native when a test suite finishes.  Override or observe to surface results in UI. */
	public static void onTestResults(String label, int passed, int total) {
		Main.Companion.onTestResults(new TestResult(label, passed, total));
	}

	/**
	 * Probe a file descriptor for PS2 BIOS metadata. Used by the setup
	 * wizard's directory-based BIOS selector to enumerate candidates and
	 * show region/version per file. The fd MUST be detached (ownership
	 * transferred to native) before the call — emucore wraps it in a FILE*
	 * and closes it on return either way.
	 *
	 * Returns null if the file isn't a valid BIOS image.
	 */
	public static native BiosInfo getBiosInfoFromFd(int fd);

	/**
	 * Read enough of a PS2 disc image to extract its serial (e.g.
	 * "SLUS-20312"). Walks the ISO9660 directory to find SYSTEM.CNF and
	 * parses the BOOT2 line. Only handles 2048-byte-sector ISOs today —
	 * CHD/CSO/etc. return null and the caller falls back to filename
	 * parsing. fd is consumed (closed by native).
	 */
	public static native String getGameSerialFromFd(int fd);

	/**
	 * PCSX2 game-database compatibility lookup. Returns the raw 0-6
	 * Compatibility enum value:
	 *   0 Unknown, 1 Nothing, 2 Intro, 3 Menu, 4 InGame, 5 Playable, 6 Perfect
	 * Caller maps to the 5-star display.
	 */
	public static native int getCompatibilityForSerial(String serial);

	public static native boolean saveStateToSlot(int slot);
	public static native boolean loadStateFromSlot(int slot);
	public static native String getGamePathSlot(int slot);
	public static native byte[] getImageSlot(int slot);

	// Autosave-on-exit slot. Backed by a dedicated `.autosave.p2s` filename
	// in the savestate folder (see VMManager::SAVESTATE_SLOT_AUTOSAVE) so the
	// numbered slots 0-9 stay user-controlled. saveAutosaveState is called
	// from the in-game "Save State And Exit" menu; hasAutosaveState gates
	// the load picker's autosave tile.
	public static native boolean saveAutosaveState();
	public static native boolean loadAutosaveState();
	public static native boolean hasAutosaveState();
	public static native byte[] getAutosaveImage();
	public static native String getAutosaveGamePath();

	public static void vmSetPaused(boolean paused) {
		if (paused)
			Main.eState.setValue(EmuState.PAUSED);
		else
			Main.eState.setValue(EmuState.RUNNING);
	}

	// Call jni
	public static int openContentUri(String uriString) {
		Context _context = getContext();
		if(_context != null) {
			ContentResolver _contentResolver = _context.getContentResolver();
			try {
				ParcelFileDescriptor filePfd = _contentResolver.openFileDescriptor(Uri.parse(uriString), "r");
				if (filePfd != null) {
					return filePfd.detachFd();  // Take ownership of the fd.
				}
			} catch (Exception ignored) {}
		}
		return -1;
	}
}
