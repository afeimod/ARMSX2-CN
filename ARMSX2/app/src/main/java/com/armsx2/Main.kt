package com.armsx2

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Process
import android.provider.Settings
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.ActivityResult
import androidx.activity.result.contract.ActivityResultContracts.StartActivityForResult
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.focusable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Text
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.armsx2.events.TestResult
import com.armsx2.ui.Colors
import com.armsx2.ui.SetupImpl
import com.armsx2.ui.WindowImpl
import compose.icons.LineAwesomeIcons
import compose.icons.lineawesomeicons.Android
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.launch
import org.libsdl.app.HIDDeviceManager
import kr.co.iefriends.pcsx2.MainActivity
import kr.co.iefriends.pcsx2.NativeApp
import org.libsdl.app.SDLControllerManager
import java.io.File
import java.io.IOException
import java.util.concurrent.Executors
import kotlin.math.min

class SurfaceCallbacks(context: Context) : SurfaceView(context), SurfaceHolder.Callback {
    init {
        holder.addCallback(this)
    }
    override fun surfaceCreated(holder: SurfaceHolder) {}
    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        NativeApp.onNativeSurfaceChanged(holder.surface, width, height)
    }
    override fun surfaceDestroyed(holder: SurfaceHolder) {
        NativeApp.onNativeSurfaceChanged(null, 0, 0)
    }

}

private const val STICK_DEAD = 0.15f

val codeGenTests = mutableStateOf("")
val patchTests = mutableStateOf("")
val vuJitTests = mutableStateOf("")
val eeJitTests = mutableStateOf("")
val vifTests = mutableStateOf("")
val eeSeqTests = mutableStateOf("")

/**
 * Gate UI shown when the user's system folder is outside the app-private
 * sandbox and MANAGE_EXTERNAL_STORAGE hasn't been granted. Blocks the main
 * emulator UI until the user either flips the toggle in Settings or opts
 * to fall back to the app-private folder.
 */
@androidx.compose.runtime.Composable
fun AllFilesAccessScreen(onGrant: () -> Unit, onUseAppPrivate: () -> Unit) {
    Box(
        Modifier.fillMaxSize().background(Colors.surface.value),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            Modifier.fillMaxSize().padding(32.dp),
            verticalArrangement = androidx.compose.foundation.layout.Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                "Storage Access Required",
                color = Color.White,
                fontSize = 26.sp,
                fontWeight = androidx.compose.ui.text.font.FontWeight.Bold,
            )
            Spacer(Modifier.size(12.dp))
            Text(
                "Your chosen system folder is outside the app's private storage. " +
                "Android requires the \"All files access\" permission to read and write " +
                "memory cards, save states, shaders, and logs there.",
                color = Color.LightGray,
                fontSize = 14.sp,
                textAlign = androidx.compose.ui.text.style.TextAlign.Center,
            )
            Spacer(Modifier.size(24.dp))
            androidx.compose.material3.Button(
                onClick = onGrant,
                colors = androidx.compose.material3.ButtonDefaults.buttonColors(
                    containerColor = Colors.pasx2_blue,
                    contentColor = Color.White,
                ),
                shape = androidx.compose.foundation.shape.RoundedCornerShape(8.dp),
                contentPadding = androidx.compose.foundation.layout.PaddingValues(
                    horizontal = 18.dp, vertical = 10.dp),
            ) {
                Text("Grant Access")
            }
            Spacer(Modifier.size(12.dp))
            androidx.compose.material3.Button(
                onClick = onUseAppPrivate,
                colors = androidx.compose.material3.ButtonDefaults.buttonColors(
                    containerColor = Color(0xFF333333),
                    contentColor = Color.White,
                ),
                shape = androidx.compose.foundation.shape.RoundedCornerShape(8.dp),
                contentPadding = androidx.compose.foundation.layout.PaddingValues(
                    horizontal = 18.dp, vertical = 10.dp),
            ) {
                Text("Use App-Private Folder Instead")
            }
            Spacer(Modifier.size(20.dp))
            Text(
                "Tip: after granting, return here — access takes effect immediately.",
                color = Color(0xFF888888),
                fontSize = 12.sp,
                textAlign = androidx.compose.ui.text.style.TextAlign.Center,
            )
        }
    }
}

class Main: ComponentActivity() {
    companion object {
        var instance : Main? = null
        lateinit var prefs: SharedPreferences
        val setupComplete = mutableStateOf(false)
        // Tree URI of the user-picked PCSX2 system folder (where bios/,
        // memcards/, etc. should live). Persisted as `systemDir` pref.
        // When unset, emucore falls back to getExternalFilesDir(null)
        // (Android/data/<package>/files).
        val systemDir = mutableStateOf<String?>(null)
        val bios = mutableStateOf<String?>(null)
        // Tree URI of the folder the user picked their BIOS from. Persisted
        // separately from `bios` (the path of the copied private file) so
        // re-entering setup can re-scan the original folder without
        // forcing the user to re-pick.
        val biosDir = mutableStateOf<String?>(null)
        val romsDir = mutableStateOf<String?>(null)

        // Renderer + upscale picked in the setup wizard. Persisted as
        // `renderer` ("opengl" / "vulkan") and `upscale` (Int 1..5). Applied
        // via NativeApp before each VM launch so the setup choices stick
        // across BIOS / game boots.
        val renderer = mutableStateOf("opengl")
        val upscale = mutableStateOf(1)

        /**
         * Tracks Android 11+ "All files access" (MANAGE_EXTERNAL_STORAGE)
         * grant. Required when the user's `systemDir` points outside the
         * app-private sandbox: emucore writes memcards / savestates /
         * configs / shaders / patches.zip / logs via raw `fopen`/`mkdir`,
         * which scoped storage rejects with EACCES even when the SAF
         * persistable URI was granted. Refreshed on Activity.onResume so
         * a return-from-Settings flips this true automatically.
         *
         * Always true on pre-Android 11 (scoped storage doesn't apply).
         */
        val allFilesAccessGranted = mutableStateOf(true)

        /** True iff Build.VERSION.SDK_INT >= R AND not granted. Pre-R the
         *  legacy WRITE_EXTERNAL_STORAGE grant covers things, so we never
         *  prompt. Used by the UI to decide whether to show the grant
         *  screen. */
        fun needsAllFilesAccess(): Boolean {
            return Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
                   !Environment.isExternalStorageManager()
        }

        /** Returns true iff the resolved system dir is the app-private
         *  externalFilesDir (or unset → defaults to it). When this is
         *  true the MANAGE_EXTERNAL_STORAGE grant isn't needed; emucore
         *  writes are inside the scoped sandbox. */
        fun systemDirIsAppPrivate(context: Context): Boolean {
            val resolved = systemDirPosix() ?: return true
            val privateRoot = context.getExternalFilesDir(null)?.absolutePath ?: return false
            return resolved.startsWith(privateRoot)
        }

        /** Open Settings → "All files access" for this app so the user
         *  can flip the toggle. Returns true if the intent launched. */
        fun requestAllFilesAccess(context: Context): Boolean {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return false
            return try {
                val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)
                intent.data = Uri.parse("package:${context.packageName}")
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                context.startActivity(intent)
                true
            } catch (_: Exception) {
                // Fallback to the all-apps version of the screen on devices
                // whose ROM rejects the package-targeted variant.
                try {
                    val intent = Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)
                    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    context.startActivity(intent)
                    true
                } catch (_: Exception) { false }
            }
        }

        private val eDispatcher = Executors.newSingleThreadExecutor().asCoroutineDispatcher()
        private val eScope = CoroutineScope(eDispatcher)

        /**
         * Resolve the user-chosen system folder (a SAF tree URI persisted
         * as `systemDir`) to a POSIX path emucore can use as
         * `EmuFolders::DataRoot`. Memcards / savestates / configs land
         * under it.
         *
         * Tree URIs from OpenDocumentTree look like
         *   content://com.android.externalstorage.documents/tree/primary%3APCSX2
         * The "primary:" prefix means the volume is the primary external
         * storage (`/storage/emulated/0`); other prefixes are SD-card or
         * removable volume IDs which mount under `/storage/<volumeId>`.
         *
         * Returns null when systemDir is unset, malformed, or this
         * Android build can't translate the tree URI (rare). Caller
         * falls back to the app's externalFilesDir in that case.
         *
         * Caveat: emucore's POSIX FileSystem APIs require the resolved
         * path to be writable. On Android 11+ that's usually only true
         * for app-owned scoped paths. Folders the user picks elsewhere
         * may translate fine but fail on write — that's a SAF-vs-POSIX
         * gap, not a translation bug.
         */
        fun systemDirPosix(): String? {
            val raw = systemDir.value ?: return null
            val uri = try { android.net.Uri.parse(raw) } catch (_: Exception) { return null }
            val docId = try {
                android.provider.DocumentsContract.getTreeDocumentId(uri)
            } catch (_: Exception) { null } ?: return null
            val parts = docId.split(":", limit = 2)
            if (parts.size != 2) return null
            val (volumeId, relPath) = parts
            return when (volumeId) {
                "primary" -> "/storage/emulated/0/$relPath"
                else -> "/storage/$volumeId/$relPath"
            }
        }

        val surface = mutableStateOf<SurfaceView?>(null)

        @JvmField
        val eState = mutableStateOf(EmuState.STOPPED)

        // Cached metadata for the currently-running game. Populated when
        // GamesList taps a card (so we have title, serial, compatibility,
        // extension and the cover URL ready), cleared when the user
        // launches via paths that don't have a GameInfo handy (Change Disc
        // file picker, BIOS-only boot). InGameOverlay reads this for its
        // top-left game info block — falls back to NativeApp.getPause* +
        // a runtime compat lookup when it's null.
        val currentGame = mutableStateOf<GameInfo?>(null)

        val focusRequester = FocusRequester()

        private var m_szGamefile = ""

        fun onTestResults(result: TestResult) {
            when (result.name) {
                "VuJitTests" -> vuJitTests.value = "${result.passed}/${result.total}"
                "PatchTests" -> patchTests.value = "${result.passed}/${result.total}"
                "CodegenTests" -> codeGenTests.value = "${result.passed}/${result.total}"
                "EeJitTests" -> eeJitTests.value = "${result.passed}/${result.total}"
                "VifTests" -> vifTests.value = "${result.passed}/${result.total}"
                "EeSeqTests" -> eeSeqTests.value = "${result.passed}/${result.total}"
                else -> println("Test:${result.name}: ${result.passed}/${result.total}")
            }
        }

        fun invoke(task: suspend () -> Unit) {
            eScope.launch {
                task()
            }
        }

        fun start() {
            invoke {
                eState.value = EmuState.RUNNING
                applyRendererPrefs()
                NativeApp.runVMThread(m_szGamefile)
                // runVMThread blocks until the VM exits (Stopping/Shutdown
                // observed). Drop back to STOPPED so the GamesList overlay
                // reappears — otherwise a failed Initialize leaves eState
                // stuck at RUNNING and the UI looks hung.
                eState.value = EmuState.STOPPED
            }
            WindowImpl.toolbarVisible.value = false
        }

        /** Push setup-wizard renderer/upscale choices into emucore's
         *  EmuConfig before runVMThread. ApplySettings inside Initialize
         *  picks them up; if a VM is already up, the JNI helpers also
         *  call MTGS::ApplySettings inline.
         *
         *  Also resolves and applies the per-game / global Settings from
         *  ConfigStore (MTVU and friends) — currentGame.serial picks the
         *  right override tier; null falls back to global. Resolution
         *  order: per-game JSON overlay → global → hardcoded defaults. */
        private fun applyRendererPrefs() {
            NativeApp.renderUpscalemultiplier(upscale.value.toFloat())
            when (renderer.value) {
                "vulkan" -> NativeApp.renderVulkan()
                else -> NativeApp.renderOpenGL()
            }
            com.armsx2.config.ConfigStore
                .resolveForGame(currentGame.value?.serial)
                .applyTo()
        }

        /**
         * Set the active game path/URI and restart the VM. Used by
         * GamesList card taps — the URI comes from the user's persisted
         * ROMs tree (already has read perm) so emucore's FileSystem
         * routines can open it via the content:// JNI bridge.
         *
         * `info` is the GameInfo from the library scan when available;
         * stored on Main.currentGame so the in-game overlay can show
         * cover art / extension badge / pre-resolved compat stars
         * without re-querying gamedb. Pass null when launching from a
         * path that doesn't have a GameInfo (Change Disc file picker).
         */
        fun launchGame(uri: String, info: GameInfo? = null) {
            currentGame.value = info
            m_szGamefile = uri
            restart()
        }

        /**
         * Boot to BIOS (no game disc). Unlike `start()` this does NOT
         * hide the toolbar — the BIOS card in GamesList wants the
         * library/toolbar to remain visible so the user can pick a game
         * once BIOS finishes booting.
         */
        fun startBios() {
            currentGame.value = null
            m_szGamefile = ""
            invoke {
                eState.value = EmuState.RUNNING
                applyRendererPrefs()
                NativeApp.runVMThread(m_szGamefile)
                eState.value = EmuState.STOPPED
            }
        }

        fun pause() {
            NativeApp.pause()
            eState.value = EmuState.PAUSED
        }

        fun resume() {
            NativeApp.resume()
            eState.value = EmuState.RUNNING
        }

        fun stop() {
            NativeApp.shutdown()
            eState.value = EmuState.STOPPED
        }

        fun restart() {
            stop()
            start()
        }

        fun renderOpenGL() {
            NativeApp.renderOpenGL()
        }

        fun renderVulkan() {
            NativeApp.renderVulkan()
        }

        fun renderSoftware() {
            NativeApp.renderSoftware()
        }

        fun copyAssetAll(p_context: Context, srcPath: String) {
            val assetMgr = p_context.assets
            try {
                val destPath =
                    p_context.getExternalFilesDir(null).toString() + File.separator + srcPath
                assetMgr.list(srcPath)?.let {
                    if (it.isEmpty()) {
                        MainActivity.copyFile(p_context, srcPath, destPath)
                    } else {
                        val dir = File(destPath)
                        if (!dir.exists()) dir.mkdir()
                        for (element in it) {
                            copyAssetAll(p_context, srcPath + File.separator + element)
                        }
                    }
                }
            } catch (ignored: IOException) {
            }
        }

        fun getSupportedGLESVersion(context: Context): Double {
            val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            val info = am.deviceConfigurationInfo
            return info.glEsVersion.toDouble()
        }

        fun isAndroidEmulator(): Boolean {
            return Build.MODEL.startsWith("sdk_")
        }
    }

    val openFileAction = registerForActivityResult(
        StartActivityForResult()
    ) { result: ActivityResult ->
        if (result.resultCode == RESULT_OK) {
            try {
                val intent = result.data
                if (intent != null) {
                    m_szGamefile = intent.dataString ?: ""
                    if (m_szGamefile.isNotEmpty()) {
                        // No GameInfo on this path — overlay's game header
                        // falls back to NativeApp.getPause* + a runtime
                        // compat lookup. Cover art will be missing.
                        currentGame.value = null
                        println(m_szGamefile)
                        restart()
                    }
                }
            } catch (_: Exception) { }
        }
    }

    init {
        instance = this
    }

    var tested = false

    fun sendKeyAction(p_action: KeyEventType, p_keycode: Int) {
        if (p_action == KeyEventType.KeyDown) {
            var pad_force = 0
            if (p_keycode >= 110) {
                var _abs = 90f // Joystic test value
                _abs = min(_abs, 100f)
                pad_force = (_abs * 32766.0f / 100).toInt()
            }
            NativeApp.setPadButton(p_keycode, pad_force, true)
        } else if (p_action == KeyEventType.KeyUp || p_action == KeyEventType.Unknown) {
            NativeApp.setPadButton(p_keycode, 0, false)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = applicationContext.getSharedPreferences("ARMSX2", MODE_PRIVATE)
        setupComplete.value = prefs.getBoolean("setupComplete", false)
        systemDir.value = prefs.getString("systemDir", null)
        bios.value = prefs.getString("bios", null)
        biosDir.value = prefs.getString("biosDir", null)
        romsDir.value = prefs.getString("roms", null)
        renderer.value = prefs.getString("renderer", "opengl") ?: "opengl"
        upscale.value = prefs.getInt("upscale", 1)
        allFilesAccessGranted.value = !needsAllFilesAccess()
        surface.value = SurfaceCallbacks(this)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).let { controller ->
            controller.hide(WindowInsetsCompat.Type.systemBars())
            controller.systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

       // Default resources
        copyAssetAll(applicationContext, "bios");
        copyAssetAll(applicationContext, "resources");

        invoke {
            NativeApp.initializeOnce(applicationContext)

            // Set up JNI
            SDLControllerManager.nativeSetupJNI()

            // Initialize state
            SDLControllerManager.initialize()

            HIDDeviceManager(applicationContext)

            println("PCSX2_INIT")

            // Tests that need VTLB/eeMem — run after init
            NativeApp.runEeJitTests()
            NativeApp.runEeSeqTests()
            NativeApp.runVifTests()
        }

        val glVersion = getSupportedGLESVersion(this)

        if (glVersion < 3.1) {
            eState.value = EmuState.RENDER_UNSUPPORTED
            println("RENDER_UNSUPPORTED")
        }

        if (isAndroidEmulator()) {
            eState.value = EmuState.EMULATOR_UNSUPPORTED
            println("DEVICE_UNSUPPORTED")
        }
        setContent {
            val ctx = androidx.compose.ui.platform.LocalContext.current
            // Setup wizard runs once. After it persists prefs and flips
            // setupComplete the main emulator UI takes over. Re-entering
            // setup requires clearing app data (or wiping the prefs key).
            //
            // Permission gate: if the user picked a system folder outside
            // the app-private sandbox (e.g. /storage/emulated/0/ARMSX2/),
            // emucore's POSIX writes need MANAGE_EXTERNAL_STORAGE. Block
            // the main UI behind a grant-request screen until the user
            // toggles it on in Settings — onResume will flip the state
            // and let the main UI through automatically.
            val needsGate = setupComplete.value &&
                !allFilesAccessGranted.value &&
                !systemDirIsAppPrivate(ctx)
            if (needsGate) {
                AllFilesAccessScreen(
                    onGrant = { requestAllFilesAccess(ctx) },
                    onUseAppPrivate = {
                        // Drop the user's chosen systemDir and fall back to
                        // app-private. emucore reverts to writing under
                        // getExternalFilesDir on next VM boot.
                        systemDir.value = null
                        prefs.edit().remove("systemDir").apply()
                    },
                )
            } else if (setupComplete.value) {
                WindowImpl.Window {
                    if (!tested) {
                        NativeApp.runCodegenTests()
                        NativeApp.runPatchTests()
                        NativeApp.runVuJitTests()
                        tested = true
                    }
                    if (surface.value != null) {
                        AndroidView(factory = { surface.value!! }, modifier = Modifier
                            .focusable()
                            .focusRequester(focusRequester)
                            .fillMaxSize()
                            .pointerInput(Unit) {
                                // Long-press on the surface while running pops the
                                // in-game pause overlay (Resume / Save+Load State /
                                // Change Disc / Library / Renderer / Frame Limit /
                                // Reset / Close — see InGameOverlay). Opening
                                // auto-pauses the VM; close paths inside the
                                // overlay handle resume themselves.
                                detectTapGestures(onLongPress = {
                                    if (eState.value == EmuState.RUNNING ||
                                        eState.value == EmuState.PAUSED) {
                                        com.armsx2.ui.InGameOverlay.open()
                                    } else {
                                        WindowImpl.toolbarVisible.value = !WindowImpl.toolbarVisible.value
                                    }
                                })
                            }
                            .onKeyEvent { event ->
                                if (eState.value != EmuState.RUNNING)
                                    return@onKeyEvent false
                                when (event.key) {
                                    // DirectionUp/Down/Left/Right intentionally NOT
                                    // bound here — they're handled by the focus
                                    // navigation path so the toolbar can use them.
                                    Key.ButtonA -> { sendKeyAction(event.type, KeyEvent.KEYCODE_BUTTON_A); true }
                                    Key.ButtonB -> { sendKeyAction(event.type, KeyEvent.KEYCODE_BUTTON_B); true }
                                    Key.ButtonX -> { sendKeyAction(event.type, KeyEvent.KEYCODE_BUTTON_X); true }
                                    Key.ButtonY -> { sendKeyAction(event.type, KeyEvent.KEYCODE_BUTTON_Y); true }
                                    Key.ButtonSelect -> { sendKeyAction(event.type, KeyEvent.KEYCODE_BUTTON_SELECT); true }
                                    Key.ButtonStart -> { sendKeyAction(event.type, KeyEvent.KEYCODE_BUTTON_START); true }
                                    Key.ButtonL1 -> { sendKeyAction(event.type, KeyEvent.KEYCODE_BUTTON_L1); true }
                                    Key.ButtonR1 -> { sendKeyAction(event.type, KeyEvent.KEYCODE_BUTTON_R1); true }
                                    Key.ButtonL2 -> { sendKeyAction(event.type, KeyEvent.KEYCODE_BUTTON_L2); true }
                                    Key.ButtonR2 -> { sendKeyAction(event.type, KeyEvent.KEYCODE_BUTTON_R2); true }
                                    Key.ButtonThumbLeft -> { sendKeyAction(event.type, KeyEvent.KEYCODE_BUTTON_THUMBL); true }
                                    Key.ButtonThumbRight -> { sendKeyAction(event.type, KeyEvent.KEYCODE_BUTTON_THUMBR); true }
                                    else -> false
                                }
                            })
                    }

                    if (eState.value == EmuState.STOPPED || eState.value == EmuState.RENDER_UNSUPPORTED || eState.value == EmuState.EMULATOR_UNSUPPORTED) {
                        Box(Modifier
                            .fillMaxSize()
                            .background(Colors.surface.value)) {
                            if (eState.value == EmuState.EMULATOR_UNSUPPORTED) {
                                Box(Modifier.align(Alignment.Center)) {
                                    Column {
                                        Image(LineAwesomeIcons.Android, "",
                                            colorFilter = ColorFilter.tint(Colors.pasx2_blue),
                                            modifier = Modifier
                                                .size(150.dp)
                                                .align(Alignment.CenterHorizontally)
                                        )
                                        Text(
                                            "Android Emulator is not supported", fontSize = 22.sp, color = Colors.pasx2_blue,
                                            modifier = Modifier.align(Alignment.CenterHorizontally)
                                        )
                                        Text(
                                            "Please use a physical device", fontSize = 22.sp, color = Colors.pasx2_blue,
                                            modifier = Modifier.align(Alignment.CenterHorizontally)
                                        )
                                    }
                                }
                            } else {
                                // Games list — replaces the old runtime-test panel.
                                // The tests still run automatically on first composition
                                // (above); their results are now available via the bug
                                // toolbar button instead of taking up the main screen.
                                com.armsx2.ui.GamesList.GamesRow()
                            }
                        }
                    }
                }
            } else {
                SetupImpl.SetupWindow()
            }
        }
    }

    override fun dispatchGenericMotionEvent(ev: MotionEvent): Boolean {
        if (eState.value == EmuState.RUNNING) {
            sendAxis(ev, MotionEvent.AXIS_X,     posCode = 111, negCode = 113) // L right / left
            sendAxis(ev, MotionEvent.AXIS_Y,     posCode = 112, negCode = 110) // L down  / up
            sendAxis(ev, MotionEvent.AXIS_Z,     posCode = 121, negCode = 123) // R right / left
            sendAxis(ev, MotionEvent.AXIS_RZ,    posCode = 122, negCode = 120) // R down  / up
            sendAxis(ev, MotionEvent.AXIS_HAT_X, posCode = 22,  negCode = 21)  // D right / left
            sendAxis(ev, MotionEvent.AXIS_HAT_Y, posCode = 20,  negCode = 19)  // D down  / up
            return true
        }
        return super.dispatchGenericMotionEvent(ev)
    }

    private fun sendAxis(event: MotionEvent, axis: Int, posCode: Int, negCode: Int) {
        val v = event.getAxisValue(axis)
        val posVal = if (v > STICK_DEAD) v else 0f
        val negVal = if (v < -STICK_DEAD) -v else 0f
        NativeApp.setPadButton(posCode, (posVal * 32767).toInt(), posVal > 0f)
        NativeApp.setPadButton(negCode, (negVal * 32767).toInt(), negVal > 0f)
    }

    override fun onPause() {
        if (eState.value == EmuState.RUNNING)
            NativeApp.pause()
        super.onPause()
    }

    override fun onResume() {
        if (eState.value == EmuState.PAUSED)
            NativeApp.resume()
        // Refresh the All-Files-Access state — the user may have just
        // returned from Settings after granting (or revoking) the
        // MANAGE_EXTERNAL_STORAGE toggle. The setContent block reads
        // this to decide whether to show the grant screen.
        allFilesAccessGranted.value = !needsAllFilesAccess()
        super.onResume()
    }

    override fun onDestroy() {
        NativeApp.shutdown()
        super.onDestroy()

        val appPid = Process.myPid()
        Process.killProcess(appPid)
    }
}