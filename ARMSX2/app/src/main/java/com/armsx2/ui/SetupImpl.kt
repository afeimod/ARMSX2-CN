package com.armsx2.ui

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.ParcelFileDescriptor
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import com.armsx2.CustomDriver
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.documentfile.provider.DocumentFile
import com.armsx2.BiosInfo
import com.armsx2.Main
import com.armsx2.R
import kr.co.iefriends.pcsx2.NativeApp
import java.io.File

object SetupImpl {
    val setupState = mutableStateOf(0)
    val allowPrev = mutableStateOf(false)
    val allowNext = mutableStateOf(false)

    /** Build version string read at first use from BuildVersion::GitRev
     *  via NativeApp.getBuildVersion(). Format
     *  "GitTagHi.GitTagMid.GitTagLo.ARMSX2Build-SNAPSHOT". The wizard
     *  shows this under the "ARMSX2" wordmark in the title row so it
     *  matches the pause overlay's brand header without a Kotlin-side
     *  hardcoded copy. */
    private val buildVersionString: String by lazy {
        runCatching { NativeApp.getBuildVersion() }
            .getOrNull()
            ?.takeIf { it.isNotEmpty() }
            ?.let { "v$it" }
            ?: ""
    }

    // -------- BIOS setup state --------
    data class ScannedBios(val uri: Uri, val displayName: String, val info: BiosInfo)
    private val scannedBioses = mutableStateListOf<ScannedBios>()
    /** null = no selection; otherwise a valid index into scannedBioses. */
    private val selectedBiosIdx = mutableStateOf<Int?>(null)
    private val biosScanning = mutableStateOf(false)
    private val biosScanError = mutableStateOf<String?>(null)

    /** Tree URI of the BIOS folder the user picked. Persisted to prefs as
     *  `biosDir` after a successful scan so re-entry can rescan without
     *  forcing a re-pick. Falls back to "no folder selected yet" UI when
     *  null (fresh first-time setup) or when a stored URI's permission
     *  has been revoked between sessions. */
    private val biosDirUri = mutableStateOf<Uri?>(null)

    /** Last folder we issued a scan for. Drives the "fire scan once per
     *  picked folder" guard in SetupBiosContent's LaunchedEffect — without
     *  this we'd double-scan on every recomposition. */
    private val lastScannedDir = mutableStateOf<Uri?>(null)

    /** Cached metadata for the configured BIOS file at `Main.bios.value`,
     *  used as a single-row fallback when we don't have a folder URI to
     *  rescan (e.g. upgrade path from an older build that didn't store
     *  biosDir, or revoked persistable permission). */
    private val configuredBiosInfo = mutableStateOf<BiosInfo?>(null)

    // -------- System dir setup state --------
    private val systemDirUri = mutableStateOf<Uri?>(null)
    private val systemDirDisplay = mutableStateOf<String?>(null)
    /** Sentinel: user explicitly picked the app-private fallback instead
     *  of a SAF folder. Treated as a valid "done" state for advancing
     *  past the system-dir step on first-run. */
    private val systemDirUseDefault = mutableStateOf(false)
    /** Surface message shown on the system-dir page when validation fails
     *  (typically scoped-storage write rejection on a non-app-private
     *  folder without MANAGE_EXTERNAL_STORAGE). null = no error. */
    private val systemDirError = mutableStateOf<String?>(null)

    // -------- ROMs dirs setup state --------
    /** Working list of ROM-folder URIs while the wizard is open. Persists
     *  to Main.romsDirs (and prefs) on Next at the ROMs step. The user
     *  can add multiple folders + remove individual entries. */
    private val romsDirsState = mutableStateListOf<Uri>()

    // -------- Renderer setup state --------
    /** Picked renderer backend, as the canonical `Main.renderer` strings
     *  ("opengl" or "vulkan"). null = nothing picked yet, blocks Next on
     *  this page. Re-entry pre-loads the existing pref so the user sees
     *  their current choice highlighted. There is no "auto" choice in the
     *  wizard — the in-game Renderer pill stays as the override path, but
     *  the wizard forces an explicit GL/VK decision so the SW path knows
     *  which display backend to host the software frame on. */
    private val selectedRenderer = mutableStateOf<String?>(null)

    // -------- Custom Vulkan driver state --------
    /** Active driver id (matches `CustomDriver.InstalledDriver.id`). null
     *  = system / default Vulkan loader. Mirrored from / to `Main.customDriverId`
     *  + prefs on commit. Persisted only when the user advances past the
     *  renderer page so abandoning the wizard mid-selection doesn't stomp
     *  a previously-saved pick. */
    private val selectedDriverId = mutableStateOf<String?>(null)

    /** Live list of `<externalFilesDir>/drivers/[id]/` entries. Refreshed
     *  on renderer-page enter, after an install, and after a delete. */
    private val installedDrivers = mutableStateListOf<CustomDriver.InstalledDriver>()

    /** Sheet visibility for the driver browser (the "+ Add" path). */
    private val showDriverBrowser = mutableStateOf(false)

    /** GitHub releases list. null = not yet fetched / loading; empty
     *  list = fetch returned nothing (or failed silently). */
    private val remoteDrivers = mutableStateOf<List<CustomDriver.RemoteDriver>?>(null)

    /** Id of the driver currently downloading + extracting (matches
     *  `CustomDriver.RemoteDriver.id`). null = no install in flight. */
    private val installingDriverId = mutableStateOf<String?>(null)

    /** Last fetch error message, surfaced as a banner at the top of the
     *  driver browser sheet. */
    private val driverBrowserError = mutableStateOf<String?>(null)

    // ---- Persist + advance helpers ----

    /**
     * Copy the user-selected BIOS into the app's private bios/ directory so
     * emucore (which reads via FileSystem::OpenManagedCFile) can load it from
     * a real path. Returns the absolute path on success, null on failure.
     */
    private fun finishBiosStep(context: Context): String? {
        val idx = selectedBiosIdx.value
        // No selection — keep what's already configured (re-entry path
        // where the scan failed but a previous BIOS was set).
        if (idx == null) return Main.bios.value

        val bios = scannedBioses.getOrNull(idx) ?: return null

        val biosDir = File(context.getExternalFilesDir(null), "bios").apply { mkdirs() }
        val outFile = File(biosDir, bios.displayName)

        // Same-content fast-path: when the user re-entered setup and the
        // pre-selected row matches the already-configured BIOS by both
        // filename and the existing private file already exists, skip the
        // copy. The pref already points at outFile and emucore is happy.
        //
        // We DON'T push setSetting from finishBiosStep on a clean install —
        // kickoffEmucoreInit is gated on setupComplete, so Java_..._initialize
        // hasn't run yet and the base settings layer isn't installed. Calling
        // Host::SetBaseStringSettingValue with no LAYER_BASE installed
        // null-derefs in LayeredSettingsInterface. The pin is now applied
        // post-init via Main.kickoffEmucoreInit's pushBiosFilenamePin().
        if (outFile.absolutePath == Main.bios.value && outFile.exists()) {
            configuredBiosInfo.value = bios.info
            return outFile.absolutePath
        }

        return try {
            context.contentResolver.openInputStream(bios.uri)?.use { ins ->
                outFile.outputStream().use { outs -> ins.copyTo(outs) }
            } ?: return null
            Main.bios.value = outFile.absolutePath
            Main.prefs.edit().putString("bios", outFile.absolutePath).apply()
            configuredBiosInfo.value = bios.info

            // BIOS filename pin (setSetting "Filenames/BIOS") is deferred —
            // see comment above. kickoffEmucoreInit reads Main.bios.value
            // after Java_..._initialize completes and pushes the filename
            // then.
            outFile.absolutePath
        } catch (_: Exception) {
            null
        }
    }

    /**
     * Open the configured BIOS file and ask emucore for its metadata. Used
     * to populate the "Currently configured" row on the BIOS page so the
     * user sees flag/version/description for what's already set up, not
     * just a filename. Local-file path → ParcelFileDescriptor → fd → JNI
     * (the same getBiosInfoFromFd path used during folder scans).
     */
    private fun probeExistingBios(path: String): BiosInfo? {
        return try {
            val pfd = ParcelFileDescriptor.open(File(path), ParcelFileDescriptor.MODE_READ_ONLY)
            val fd = pfd.detachFd()
            NativeApp.getBiosInfoFromFd(fd)
        } catch (_: Exception) {
            null
        }
    }

    private fun finishSystemDirStep(context: Context): String? {
        // App-private fallback path. Wipe any prior systemDir pref so
        // NativeApp.initializeOnce → Main.systemDirPosix returns null and
        // emucore writes under getExternalFilesDir.
        if (systemDirUseDefault.value) {
            Main.systemDir.value = null
            Main.prefs.edit().remove("systemDir").apply()
            systemDirError.value = null
            return context.getExternalFilesDir(null)?.absolutePath
                ?: context.dataDir.absolutePath
        }

        val uri = systemDirUri.value
        // Re-entry path: keep existing pref if user didn't repick. The
        // persistable grant from a prior session survives across process
        // restarts so we don't need to re-take it.
        if (uri == null) return Main.systemDir.value

        // Validate POSIX writability BEFORE persisting. The SAF
        // tree-URI grant lets us read, but emucore's FileSystem APIs
        // hit raw fopen/mkdir which scoped storage rejects on
        // Android 11+ unless MANAGE_EXTERNAL_STORAGE is granted.
        // Without this gate, the wizard finishes happily, the user
        // boots a game, and emucore SIGSEGVs trying to gen memcards
        // / savestates / configs in a non-writable dir.
        val posix = Main.resolveTreeUriToPosix(uri.toString())
        if (posix != null && !Main.validateSystemDirWritable(posix)) {
            // Auto-open the grant screen on Android 11+ so the user can
            // toggle the permission with one tap. Activity.onResume will
            // refresh allFilesAccessGranted; user re-clicks Next.
            if (Main.needsAllFilesAccess()) {
                Main.requestAllFilesAccess(context)
                systemDirError.value = "Can't write to that folder. Grant " +
                    "All Files Access (just opened in Settings), then tap Next again. " +
                    "Or use the App-Private Folder option below."
            } else {
                // Permission already granted (or pre-Android-11) but the
                // path still rejected writes — likely a removable / SD-
                // card path the device doesn't surface as POSIX. Push
                // the user to the fallback.
                systemDirError.value = "That folder isn't writable from native code. " +
                    "Pick a different folder or use the App-Private Folder option below."
            }
            return null
        }

        try {
            context.contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        } catch (_: SecurityException) { /* already persisted, or revoked */ }
        Main.systemDir.value = uri.toString()
        Main.prefs.edit().putString("systemDir", uri.toString()).apply()
        systemDirError.value = null
        return uri.toString()
    }

    private fun finishRomsStep(context: Context): String? {
        // Need at least one ROM folder. Re-entry from the Settings cog
        // pre-loads Main.romsDirs into romsDirsState so the user sees
        // their existing list; if they didn't change anything, we still
        // re-run the persistable-permission take (idempotent, harmless).
        if (romsDirsState.isEmpty()) return null

        for (uri in romsDirsState) {
            try {
                context.contentResolver.takePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            } catch (_: SecurityException) { /* already persisted, or revoked */ }
        }
        Main.setRomsDirs(romsDirsState.map { it.toString() })
        // Returning the first URI's string keeps the existing String?-returning
        // contract for the wizard's Next gating (non-null = success).
        return romsDirsState.first().toString()
    }

    private fun refreshAllowNext() {
        allowNext.value = when (setupState.value) {
            0 -> true
            // Renderer page — Next requires an explicit GL/VK pick. No
            // default-through; we want every user to have made a conscious
            // choice so support tickets can rely on the backend string.
            1 -> selectedRenderer.value != null
            // System dir page — Next when the user has a fresh URI selected,
            // an existing pref to keep (re-entry), or has explicitly opted
            // into the app-private fallback.
            2 -> systemDirUri.value != null ||
                 systemDirUseDefault.value ||
                 Main.systemDir.value != null
            // BIOS page — Next when a row is picked, or (fallback for revoked
            // / missing biosDir) an already-configured BIOS we're keeping.
            3 -> selectedBiosIdx.value != null ||
                 (biosDirUri.value == null && Main.bios.value != null)
            // ROMs page — Next when at least one folder is selected.
            4 -> romsDirsState.isNotEmpty()
            else -> false
        }
    }

    /**
     * Reset wizard nav state for re-entry from the Settings (cog) toolbar
     * button. Existing `Main.bios` / `Main.romsDir` prefs stay in place; the
     * ROMs URI is also pre-loaded into the page state so the user visibly
     * sees their current setting (otherwise the page would render as
     * "No ROMs folder selected yet" and look like the dir got wiped).
     * The BIOS list still requires a fresh folder pick to populate
     * `scannedBioses`; the page shows a "Currently configured" card on
     * re-entry so the existing BIOS is visible there too.
     */
    fun resetForReentry() {
        setupState.value = 0
        allowPrev.value = false
        allowNext.value = true
        // Pre-load the saved renderer pick so the wizard tile is highlighted
        // on re-entry. Treat "auto" (the historical default) as "no choice
        // yet" so re-entry from old installs still forces an explicit pick.
        selectedRenderer.value = Main.renderer.value.takeIf {
            it == "opengl" || it == "vulkan"
        }
        // Custom driver state mirrors Main.customDriverId (already hydrated
        // from prefs in onCreate). installedDrivers stays empty here; the
        // renderer page composable refreshes it on first enter so we don't
        // hit disk on every wizard reset.
        selectedDriverId.value = Main.customDriverId.value
        showDriverBrowser.value = false
        installingDriverId.value = null
        driverBrowserError.value = null
        scannedBioses.clear()
        selectedBiosIdx.value = null
        biosScanning.value = false
        biosScanError.value = null
        // Drop the last-scanned marker so the SetupBiosContent
        // LaunchedEffect re-issues a scan against the (preserved) folder.
        lastScannedDir.value = null

        // Pre-load the saved BIOS folder URI. The page's LaunchedEffect
        // picks this up and rescans; on completion the configured BIOS is
        // auto-selected by filename match. Falls through to single-row
        // configuredBiosInfo render if the URI is null or the rescan fails.
        val savedBiosDir = Main.biosDir.value
        biosDirUri.value = if (savedBiosDir != null) {
            try { Uri.parse(savedBiosDir) } catch (_: Exception) { null }
        } else null

        // Probed BIOS metadata for the fallback "no folder URI" path.
        val existingBios = Main.bios.value
        configuredBiosInfo.value = if (existingBios != null) probeExistingBios(existingBios) else null

        val existingSystem = Main.systemDir.value
        if (existingSystem != null) {
            try {
                val uri = Uri.parse(existingSystem)
                systemDirUri.value = uri
                systemDirDisplay.value = uri.lastPathSegment ?: existingSystem
            } catch (_: Exception) {
                systemDirUri.value = null
                systemDirDisplay.value = null
            }
        } else {
            systemDirUri.value = null
            systemDirDisplay.value = null
        }
        systemDirUseDefault.value = false
        systemDirError.value = null

        // Pre-load saved ROMs list. Each URI is parsed best-effort —
        // malformed entries (rare, only if prefs was hand-edited) are
        // dropped silently rather than failing the whole list.
        romsDirsState.clear()
        for (s in Main.romsDirs.value) {
            val u = try { Uri.parse(s) } catch (_: Exception) { null } ?: continue
            romsDirsState.add(u)
        }
    }

    /** Page heading shown in the upper-left of the title row. */
    private fun pageTitle(): String = when (setupState.value) {
        0 -> "Welcome"
        1 -> "Choose renderer"
        2 -> "Select system folder"
        3 -> "Select your BIOS"
        4 -> "Select ROMs folder"
        else -> ""
    }

    /** Label for the page-local action button (in the nav row). null = no button. */
    private fun midButtonLabel(): String? = when (setupState.value) {
        2 -> if (systemDirUri.value == null) "Pick System Folder" else "Pick a different folder"
        // Use the URI presence (not the in-memory list) so the label says
        // "Pick a different folder" immediately on re-entry when we already
        // have a remembered biosDir, even before the auto-rescan finishes.
        3 -> if (biosDirUri.value == null) "Pick BIOS Folder" else "Pick a different folder"
        4 -> if (romsDirsState.isEmpty()) "Pick ROMs Folder" else "Add Another Folder"
        else -> null
    }

    /** Persist the picked renderer to prefs + Main.renderer so applyRendererPrefs
     *  routes to the right JNI on the next VM start. Returns the canonical
     *  string on success, null when no choice was made (Next is gated above
     *  but defensive — re-entry edge cases). */
    private fun finishRendererStep(): String? {
        val pick = selectedRenderer.value ?: return null
        Main.renderer.value = pick
        // Driver pick is only meaningful for Vulkan; clear it on the OpenGL
        // path so re-entry from OGL doesn't leave a stale custom driver
        // pinned (the JNI setter would still fire and adrenotools would
        // resolve a path that gets ignored by the GL backend, but cleaner
        // to drop it).
        val driverId = if (pick == "vulkan") selectedDriverId.value else null
        Main.customDriverId.value = driverId
        Main.prefs.edit()
            .putString("renderer", pick)
            .putString("customDriverId", driverId.orEmpty())
            .apply()
        return pick
    }

    /** Reusable PS2-blue button colors. */
    @Composable
    private fun ps2Colors() = ButtonDefaults.buttonColors(
        containerColor = Colors.pasx2_blue,
        contentColor = Color.White,
        disabledContainerColor = Colors.pasx2_blue.copy(alpha = 0.30f),
        disabledContentColor = Color.White.copy(alpha = 0.50f),
    )

    @Composable
    fun SetupWindow() {
        val context = LocalContext.current

        val systemLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocumentTree()
        ) { treeUri: Uri? ->
            if (treeUri == null) return@rememberLauncherForActivityResult
            // System folder needs read+write — emucore writes memcards,
            // save states, and config there.
            try {
                context.contentResolver.takePersistableUriPermission(
                    treeUri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
            } catch (_: SecurityException) { /* already persisted */ }
            systemDirUri.value = treeUri
            systemDirDisplay.value = treeUri.lastPathSegment ?: treeUri.toString()
            // Picking a fresh folder cancels the app-private opt-in and
            // clears any prior validation error so the user gets a fresh
            // shot at the writability probe on Next.
            systemDirUseDefault.value = false
            systemDirError.value = null
            refreshAllowNext()
        }
        val biosLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocumentTree()
        ) { treeUri: Uri? ->
            if (treeUri == null) return@rememberLauncherForActivityResult
            // Take persistable permission so the URI survives across app
            // restarts; the LaunchedEffect in SetupBiosContent picks up the
            // change to biosDirUri and runs the actual scan.
            try {
                context.contentResolver.takePersistableUriPermission(
                    treeUri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            } catch (_: SecurityException) { /* already persisted */ }
            biosDirUri.value = treeUri
            // Force a re-scan even if biosDirUri is the same Uri value as
            // before — guards against a user picking the same folder
            // through the picker dialog after a "no results" outcome.
            lastScannedDir.value = null
        }
        val romsLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocumentTree()
        ) { treeUri: Uri? ->
            if (treeUri != null) {
                // Take the persistable grant immediately so the URI survives
                // across app restarts even if the user closes the wizard
                // before tapping Next. De-duplicate by URI string so picking
                // the same folder twice doesn't add a redundant entry.
                try {
                    context.contentResolver.takePersistableUriPermission(
                        treeUri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                } catch (_: SecurityException) { /* already persisted */ }
                if (romsDirsState.none { it.toString() == treeUri.toString() }) {
                    romsDirsState.add(treeUri)
                }
                refreshAllowNext()
            }
        }

        Box(Modifier.fillMaxSize().background(Colors.surface.value)) {
            Column(Modifier.fillMaxSize()) {

                // Title row — page heading on the left, ARMSX2 + version
                // stacked above the logo on the right. Mirrors the
                // InGameOverlay BrandHeader so wizard / pause overlay
                // share the same wordmark layout. Version comes from
                // BuildVersion::GitRev via NativeApp.getBuildVersion()
                // so it tracks the C++ constants automatically.
                Row(
                    Modifier.fillMaxWidth().height(64.dp).padding(horizontal = 16.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        pageTitle(),
                        color = Color.White,
                        fontSize = 22.sp,
                        fontWeight = FontWeight.Bold,
                    )
                    Spacer(Modifier.weight(1f))
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(
                            "ARMSX2",
                            color = Color.White,
                            fontSize = 16.sp,
                            fontWeight = FontWeight.Bold,
                        )
                        Text(
                            buildVersionString,
                            color = Color(0xFF888888),
                            fontSize = 10.sp,
                        )
                    }
                    Image(
                        painter = painterResource(id = R.drawable.savetowerforeground),
                        contentDescription = null,
                        modifier = Modifier.size(40.dp).padding(start = 8.dp),
                    )
                }

                // Content — fills remaining height. Pages no longer carry their
                // own header / picker button; SetupWindow's title row + nav row
                // own those, leaving the entire content area for the page list.
                Box(Modifier.weight(1f).fillMaxWidth().padding(horizontal = 16.dp)) {
                    when (setupState.value) {
                        0 -> {
                            allowNext.value = true
                            Welcome()
                        }
                        1 -> SetupRendererContent()
                        2 -> SetupSystemDirContent()
                        3 -> SetupBiosContent()
                        4 -> SetupRomsDirContent()
                        else -> {
                            Main.prefs.edit().putBoolean("setupComplete", true).apply()
                            Main.setupComplete.value = true
                        }
                    }
                }

                // Nav row — Prev, mid action, Next.
                Row(
                    Modifier.fillMaxWidth().height(72.dp).padding(horizontal = 16.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Button(
                        onClick = {
                            when (setupState.value) {
                                1 -> {
                                    setupState.value = 0
                                    allowPrev.value = false
                                    allowNext.value = true
                                }
                                2 -> {
                                    setupState.value = 1
                                    allowPrev.value = true
                                    refreshAllowNext()
                                }
                                3 -> {
                                    setupState.value = 2
                                    allowPrev.value = true
                                    refreshAllowNext()
                                }
                                4 -> {
                                    setupState.value = 3
                                    allowPrev.value = true
                                    refreshAllowNext()
                                }
                            }
                        },
                        enabled = allowPrev.value,
                        colors = ps2Colors(),
                    ) {
                        Text("Prev")
                    }

                    Spacer(Modifier.width(12.dp))

                    // Page-local action button (folder pickers).
                    val midLabel = midButtonLabel()
                    if (midLabel != null) {
                        Button(
                            onClick = {
                                when (setupState.value) {
                                    2 -> systemLauncher.launch(null)
                                    3 -> biosLauncher.launch(null)
                                    4 -> romsLauncher.launch(null)
                                }
                            },
                            colors = ps2Colors(),
                        ) {
                            Text(midLabel)
                        }
                    }

                    Spacer(Modifier.weight(1f))

                    Button(
                        onClick = {
                            when (setupState.value) {
                                0 -> {
                                    setupState.value = 1
                                    allowPrev.value = true
                                    refreshAllowNext()
                                }
                                1 -> {
                                    if (finishRendererStep() != null) {
                                        setupState.value = 2
                                        allowPrev.value = true
                                        refreshAllowNext()
                                    }
                                }
                                2 -> {
                                    if (finishSystemDirStep(context) != null) {
                                        setupState.value = 3
                                        allowPrev.value = true
                                        refreshAllowNext()
                                    }
                                }
                                3 -> {
                                    if (finishBiosStep(context) != null) {
                                        setupState.value = 4
                                        allowPrev.value = true
                                        refreshAllowNext()
                                    }
                                }
                                4 -> {
                                    if (finishRomsStep(context) != null) {
                                        setupState.value = 5
                                        allowPrev.value = false
                                        allowNext.value = false
                                    }
                                }
                            }
                        },
                        enabled = allowNext.value,
                        colors = ps2Colors(),
                    ) {
                        // Last setup page commits the prefs and dismisses
                        // the wizard, so the action label changes to match.
                        Text(if (setupState.value == 4) "Finish" else "Next")
                    }
                }
            }
        }
    }

    /** First-run welcome page. Upscale defaults to 1x and is exposed in the
     *  in-game overlay's Renderer tab for runtime override. Renderer is
     *  picked explicitly on the next page so the SW path knows which host
     *  display backend to host the software frame on. */
    @Composable
    fun Welcome() {
        Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.Center) {
            Text("Welcome to ARMSX2 setup!", Modifier.align(Alignment.CenterHorizontally),
                fontSize = 28.sp, color = Color.White, fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(8.dp))
            Text("Hit Next to get started", Modifier.align(Alignment.CenterHorizontally),
                fontSize = 14.sp, color = Color.LightGray)
        }
    }

    /** Renderer page — two large tile choices: OpenGL or Vulkan. The pick
     *  is the host display backend used for BOTH HW (OGL / VK) and SW
     *  (software GS, presenting via the chosen backend). The in-game
     *  overlay's Renderer pill still toggles between HW and SW within the
     *  chosen backend; switching backends after first-run requires
     *  re-entering setup from the Settings cog. */
    @Composable
    private fun SetupRendererContent() {
        val context = LocalContext.current
        // First-enter populate of installedDrivers; cheap, only hits disk
        // once per wizard session. Also re-runs when the Vulkan tile
        // becomes selected since the user might have just toggled to it.
        LaunchedEffect(selectedRenderer.value) {
            if (selectedRenderer.value == "vulkan") {
                installedDrivers.clear()
                installedDrivers.addAll(CustomDriver.listInstalled(context))
            }
        }

        // The driver browser takes over the page when open — full content
        // area, mirrors the BIOS-rescan modality.
        if (showDriverBrowser.value) {
            DriverBrowserSheet()
            return
        }

        Column(modifier = Modifier.fillMaxSize()) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                RendererTile(
                    title = "OpenGL",
                    selected = selectedRenderer.value == "opengl",
                    modifier = Modifier.weight(1f),
                    onClick = {
                        selectedRenderer.value = "opengl"
                        refreshAllowNext()
                    },
                )
                RendererTile(
                    title = "Vulkan",
                    selected = selectedRenderer.value == "vulkan",
                    modifier = Modifier.weight(1f),
                    onClick = {
                        selectedRenderer.value = "vulkan"
                        refreshAllowNext()
                    },
                )
            }

            if (selectedRenderer.value == "vulkan") {
                Spacer(Modifier.height(20.dp))
                GpuDriverSection()
            }
        }
    }

    @Composable
    private fun RendererTile(
        title: String,
        selected: Boolean,
        modifier: Modifier = Modifier,
        onClick: () -> Unit,
    ) {
        val bg = if (selected) Colors.pasx2_blue.copy(alpha = 0.30f) else Color(0xFF1F2123)
        val border = if (selected) Colors.pasx2_blue else Color.White.copy(alpha = 0.10f)
        // Smaller tiles than first-pass — the GPU Driver section needs the
        // vertical room below, and the title alone reads fine without a
        // 32dp pad.
        Box(
            modifier = modifier
                .clip(RoundedCornerShape(10.dp))
                .background(bg)
                .border(2.dp, border, RoundedCornerShape(10.dp))
                .clickable(onClick = onClick)
                .padding(vertical = 14.dp),
            contentAlignment = Alignment.Center,
        ) {
            Text(title, color = Color.White, fontSize = 20.sp, fontWeight = FontWeight.Bold)
        }
    }

    /** Horizontal scrolling list of driver chips. First chip is "Default
     *  (system loader)", then each installed driver, then a trailing "+
     *  Add" chip that opens the driver browser. */
    @Composable
    private fun GpuDriverSection() {
        Text(
            "GPU Driver",
            color = Color.White,
            fontSize = 16.sp,
            fontWeight = FontWeight.Bold,
        )
        Spacer(Modifier.height(4.dp))
        Text(
            "Replace the system Vulkan driver with Mesa Turnip or another Adreno driver. Recommended for Adreno-6XX users on stale OEM drivers. Takes effect on the next game launch.",
            color = Color(0xFFAAAAAA),
            fontSize = 12.sp,
        )
        Spacer(Modifier.height(10.dp))
        LazyRow(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            item {
                DriverChip(
                    label = "Default",
                    sublabel = "System driver",
                    detail = "Use the device's stock Vulkan ICD",
                    selected = selectedDriverId.value == null,
                    onClick = { selectedDriverId.value = null },
                )
            }
            items(installedDrivers, key = { it.id }) { drv ->
                // Pack as much as the meta.json offers — name, vendor +
                // driver version (the differentiator across releases of
                // the same family), description if any.
                val sublabel = buildString {
                    if (drv.vendor.isNotEmpty()) append(drv.vendor)
                    if (drv.version.isNotEmpty()) {
                        if (isNotEmpty()) append(" · ")
                        append(drv.version)
                    }
                    if (isEmpty() && drv.author.isNotEmpty()) append(drv.author)
                    if (isEmpty()) append("Installed")
                }
                DriverChip(
                    label = drv.name,
                    sublabel = sublabel,
                    detail = drv.description.ifBlank { drv.author },
                    selected = selectedDriverId.value == drv.id,
                    onClick = { selectedDriverId.value = drv.id },
                    onDelete = {
                        if (selectedDriverId.value == drv.id) selectedDriverId.value = null
                        CustomDriver.delete(drv)
                        installedDrivers.remove(drv)
                    },
                )
            }
            item {
                AddDriverChip(onClick = {
                    showDriverBrowser.value = true
                    // Kick off the fetch if we haven't already.
                    if (remoteDrivers.value == null) remoteDrivers.value = emptyList()
                })
            }
        }
    }

    @Composable
    private fun DriverChip(
        label: String,
        sublabel: String,
        detail: String,
        selected: Boolean,
        onClick: () -> Unit,
        onDelete: (() -> Unit)? = null,
    ) {
        val bg = if (selected) Colors.pasx2_blue.copy(alpha = 0.30f) else Color(0xFF1F2123)
        val border = if (selected) Colors.pasx2_blue else Color.White.copy(alpha = 0.10f)
        val labelColor = if (selected) Color.White else Color(0xFFE0E0E0)
        // 220dp chip is wide enough to surface the differentiator (Mesa
        // vendor + driverVersion) plus a description preview without
        // ellipsing the driver name itself. The chip row scrolls
        // horizontally so width can grow without crowding the page.
        Box(
            modifier = Modifier
                .width(220.dp)
                .clip(RoundedCornerShape(10.dp))
                .background(bg)
                .border(1.5.dp, border, RoundedCornerShape(10.dp))
                .clickable(onClick = onClick),
        ) {
            Column(modifier = Modifier.padding(horizontal = 14.dp, vertical = 12.dp)) {
                Text(
                    label,
                    color = labelColor,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(3.dp))
                Text(
                    sublabel,
                    color = Colors.pasx2_blue,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
                )
                if (detail.isNotEmpty()) {
                    Spacer(Modifier.height(4.dp))
                    Text(
                        detail,
                        color = Color(0xFF999999),
                        fontSize = 10.sp,
                        maxLines = 2,
                        overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
                    )
                }
            }
            // Small corner X for delete, only present on user-installed
            // chips (Default has no delete). Tap is independent of the chip
            // body's click handler.
            if (onDelete != null) {
                Box(
                    Modifier
                        .align(Alignment.TopEnd)
                        .padding(4.dp)
                        .size(20.dp)
                        .clip(RoundedCornerShape(10.dp))
                        .background(Color(0xFF3A1A1A))
                        .clickable(onClick = onDelete),
                    contentAlignment = Alignment.Center,
                ) {
                    Text("×", color = Color(0xFFFF6B6B), fontSize = 14.sp, fontWeight = FontWeight.Bold)
                }
            }
        }
    }

    @Composable
    private fun AddDriverChip(onClick: () -> Unit) {
        Box(
            modifier = Modifier
                .width(220.dp)
                .clip(RoundedCornerShape(10.dp))
                .background(Color(0xFF1A1A1A))
                .border(
                    1.5.dp,
                    Colors.pasx2_blue.copy(alpha = 0.5f),
                    RoundedCornerShape(10.dp))
                .clickable(onClick = onClick)
                .padding(vertical = 18.dp),
            contentAlignment = Alignment.Center,
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text("+", color = Colors.pasx2_blue, fontSize = 24.sp, fontWeight = FontWeight.Bold)
                Text(
                    "Add Driver",
                    color = Colors.pasx2_blue,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                )
            }
        }
    }

    /** Fullscreen-within-the-content driver browser. Lists releases from
     *  K11MCH1/AdrenoToolsDrivers. Tapping Install downloads + extracts
     *  into the drivers dir; on success the chip row picks it up via
     *  installedDrivers refresh. */
    @Composable
    private fun DriverBrowserSheet() {
        val context = LocalContext.current
        val scope = rememberCoroutineScope()

        // SAF picker for "pick local .zip" — adrenotools driver packs
        // from outside the GitHub list (e.g. user pre-downloaded, sideloaded).
        // MIME filter accepts both application/zip and the catch-all
        // application/octet-stream because some file providers (Drive,
        // Files-by-Google) report .zip downloads as octet-stream.
        val localLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocument()
        ) { uri: Uri? ->
            if (uri == null) return@rememberLauncherForActivityResult
            // Mark the sheet as "installing" with a synthetic id so the
            // existing busy-row indicator surfaces. Using a sentinel that
            // can't collide with any real remote id.
            val pendingId = "local-import"
            installingDriverId.value = pendingId
            scope.launch {
                val result = withContext(Dispatchers.IO) {
                    CustomDriver.installFromUri(context, uri)
                }
                installingDriverId.value = null
                if (result != null) {
                    installedDrivers.clear()
                    installedDrivers.addAll(CustomDriver.listInstalled(context))
                    selectedDriverId.value = result.id
                    driverBrowserError.value = null
                    showDriverBrowser.value = false
                } else {
                    driverBrowserError.value = "Couldn't import that file. It needs to be an AdrenoToolsDrivers-style .zip with meta.json + libvulkan_freedreno.so at the root."
                }
            }
        }

        // Fetch on first entry to the sheet. remoteDrivers stays around
        // for the wizard session so reopening the sheet is instant.
        LaunchedEffect(Unit) {
            if (remoteDrivers.value.isNullOrEmpty()) {
                driverBrowserError.value = null
                val fetched = withContext(Dispatchers.IO) { CustomDriver.fetchRemote() }
                remoteDrivers.value = fetched
                if (fetched.isEmpty()) {
                    driverBrowserError.value = "Couldn't reach github.com/K11MCH1/AdrenoToolsDrivers. Check your connection and try again."
                }
            }
        }

        Column(modifier = Modifier.fillMaxSize()) {
            Row(
                modifier = Modifier.fillMaxWidth().padding(bottom = 10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    "Available drivers",
                    color = Color.White,
                    fontSize = 18.sp,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier.weight(1f),
                )
                Box(
                    Modifier
                        .clip(RoundedCornerShape(6.dp))
                        .background(Colors.pasx2_blue.copy(alpha = 0.30f))
                        .border(1.dp, Colors.pasx2_blue.copy(alpha = 0.65f), RoundedCornerShape(6.dp))
                        .clickable {
                            localLauncher.launch(arrayOf("application/zip", "application/octet-stream", "*/*"))
                        }
                        .padding(horizontal = 12.dp, vertical = 6.dp),
                ) {
                    Text(
                        "Pick local .zip",
                        color = Color.White,
                        fontSize = 12.sp,
                        fontWeight = FontWeight.Bold,
                    )
                }
                Spacer(Modifier.width(8.dp))
                Box(
                    Modifier
                        .clip(RoundedCornerShape(6.dp))
                        .background(Color(0xFF2A2A2A))
                        .clickable { showDriverBrowser.value = false }
                        .padding(horizontal = 12.dp, vertical = 6.dp),
                ) {
                    Text("Close", color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.Bold)
                }
            }
            Text(
                "Source: github.com/K11MCH1/AdrenoToolsDrivers — Mesa Turnip and friends. Each driver lands under app-private storage.",
                color = Color(0xFF888888),
                fontSize = 11.sp,
                modifier = Modifier.padding(bottom = 10.dp),
            )

            val err = driverBrowserError.value
            if (err != null) {
                Row(
                    Modifier.fillMaxWidth()
                        .clip(RoundedCornerShape(8.dp))
                        .background(Color(0xFF5A1A1A))
                        .padding(12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("⚠", fontSize = 18.sp, color = Color(0xFFFF6B6B))
                    Spacer(Modifier.width(8.dp))
                    Text(err, color = Color.White, fontSize = 13.sp, modifier = Modifier.weight(1f))
                }
                Spacer(Modifier.height(10.dp))
            }

            val list = remoteDrivers.value
            when {
                list == null -> {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(20.dp),
                            strokeWidth = 2.dp,
                            color = Colors.pasx2_blue,
                        )
                        Spacer(Modifier.width(12.dp))
                        Text("Loading driver list…", color = Color.White, fontSize = 13.sp)
                    }
                }
                list.isEmpty() -> {
                    Text(
                        "No drivers available right now.",
                        color = Color.LightGray,
                    )
                }
                else -> {
                    // 2-column grid mirroring the BIOS picker layout.
                    LazyVerticalGrid(
                        columns = GridCells.Fixed(2),
                        modifier = Modifier.fillMaxSize(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(6.dp),
                    ) {
                        itemsIndexed(list, key = { _, it -> it.id }) { _, remote ->
                            val installed = installedDrivers.any { it.id == remote.id }
                            val installing = installingDriverId.value == remote.id
                            val active = installed && selectedDriverId.value == remote.id
                            DriverBrowserRow(
                                remote = remote,
                                installed = installed,
                                installing = installing,
                                active = active,
                                onInstall = {
                                    installingDriverId.value = remote.id
                                    scope.launch {
                                        val result = withContext(Dispatchers.IO) {
                                            CustomDriver.download(context, remote)
                                        }
                                        installingDriverId.value = null
                                        if (result != null) {
                                            // Install also activates: refresh chips,
                                            // set this driver as the active pick, and
                                            // bounce out of the sheet so the user
                                            // immediately sees their selection.
                                            installedDrivers.clear()
                                            installedDrivers.addAll(CustomDriver.listInstalled(context))
                                            selectedDriverId.value = result.id
                                            showDriverBrowser.value = false
                                        } else {
                                            driverBrowserError.value = "Install failed for ${remote.assetName}. The download or extract step errored — try again."
                                        }
                                    }
                                },
                                onSelect = {
                                    selectedDriverId.value = remote.id
                                    showDriverBrowser.value = false
                                },
                            )
                        }
                    }
                }
            }
        }
    }

    @Composable
    private fun DriverBrowserRow(
        remote: CustomDriver.RemoteDriver,
        installed: Boolean,
        installing: Boolean,
        active: Boolean,
        onInstall: () -> Unit,
        onSelect: () -> Unit,
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                .background(Color(0xFF1F2123))
                .padding(12.dp),
        ) {
            Text(
                remote.releaseName.ifBlank { remote.assetName },
                color = Color.White,
                fontSize = 14.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
                overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(2.dp))
            Text(
                remote.assetName,
                color = Color(0xFFAACCFF),
                fontSize = 11.sp,
                maxLines = 1,
                overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(4.dp))
            Text(
                buildString {
                    if (remote.tagName.isNotEmpty()) append(remote.tagName)
                    if (remote.sizeBytes > 0) {
                        if (isNotEmpty()) append(" · ")
                        append(humanBytes(remote.sizeBytes))
                    }
                    if (remote.publishedAt.isNotEmpty()) {
                        if (isNotEmpty()) append(" · ")
                        append(remote.publishedAt.substringBefore('T'))
                    }
                },
                color = Color(0xFF888888),
                fontSize = 10.sp,
                maxLines = 1,
                overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
            )
            Spacer(Modifier.height(10.dp))
            // Action button fills the cell width — visually tighter in a
            // 2-column grid than a right-aligned button.
            when {
                installing -> {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(6.dp))
                            .background(Color(0xFF2A2A2A))
                            .padding(vertical = 7.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.Center,
                    ) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(14.dp),
                            strokeWidth = 2.dp,
                            color = Colors.pasx2_blue,
                        )
                        Spacer(Modifier.width(8.dp))
                        Text("Installing…", color = Color(0xFFCCCCCC), fontSize = 11.sp, fontWeight = FontWeight.Bold)
                    }
                }
                active -> {
                    // Already the active pick — no-op tile, distinct visual
                    // so the user can see "yes you're using this one".
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(6.dp))
                            .background(Color(0xFF1F3A1F))
                            .padding(vertical = 7.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text("✓ Active", color = Color(0xFF9ED49E), fontSize = 11.sp, fontWeight = FontWeight.Bold)
                    }
                }
                installed -> {
                    // Installed but a different driver is active — Select
                    // makes this the active pick + closes the sheet.
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(6.dp))
                            .background(Colors.pasx2_blue.copy(alpha = 0.30f))
                            .border(1.dp, Colors.pasx2_blue.copy(alpha = 0.65f), RoundedCornerShape(6.dp))
                            .clickable(onClick = onSelect)
                            .padding(vertical = 7.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text("Select", color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                    }
                }
                else -> {
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(6.dp))
                            .background(Colors.pasx2_blue)
                            .clickable(onClick = onInstall)
                            .padding(vertical = 7.dp),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text("Install", color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.Bold)
                    }
                }
            }
        }
    }

    private fun humanBytes(n: Long): String {
        if (n < 1024) return "${n} B"
        if (n < 1024 * 1024) return "${n / 1024} KB"
        return "%.1f MB".format(n / 1024.0 / 1024.0)
    }

    /** BIOS page content — single scrollable list. The pick button lives in
     *  the nav row. The remembered BIOS folder URI auto-rescans on entry so
     *  the user sees the full list of BIOS images they originally picked
     *  from, with the configured one pre-selected. Fallback to a single-row
     *  view when no folder URI is available (older builds, revoked
     *  permission, or first-launch before any folder has been picked). */
    @Composable
    private fun SetupBiosContent() {
        val context = LocalContext.current

        // Cold-start hydration: process restart with setupComplete=false
        // reaches this composable directly, so biosDirUri may still be null
        // even though Main.biosDir was loaded from prefs in onCreate. Pull
        // it across so the auto-scan effect below picks it up.
        LaunchedEffect(Unit) {
            if (biosDirUri.value == null) {
                Main.biosDir.value?.let { dirStr ->
                    biosDirUri.value = try { Uri.parse(dirStr) } catch (_: Exception) { null }
                }
            }
        }

        // Auto-rescan the remembered folder. lastScannedDir is the guard:
        // resetForReentry / the picker callback both null it out so this
        // effect re-runs once per "user wants a fresh scan" event.
        LaunchedEffect(biosDirUri.value, lastScannedDir.value) {
            val uri = biosDirUri.value
            if (uri != null && uri != lastScannedDir.value && !biosScanning.value) {
                scanBiosDirectory(context, uri)
            }
        }

        // Cold-start: probe the configured BIOS so the fallback single-row
        // view (when biosDirUri is null) has metadata to display.
        val configuredPath = Main.bios.value
        LaunchedEffect(configuredPath) {
            if (configuredPath != null && configuredBiosInfo.value == null) {
                configuredBiosInfo.value = probeExistingBios(configuredPath)
            }
        }

        Column(Modifier.fillMaxSize()) {
            when {
                biosScanning.value -> {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CircularProgressIndicator(
                            modifier = Modifier.size(20.dp),
                            strokeWidth = 2.dp,
                            color = Colors.pasx2_blue,
                        )
                        Spacer(Modifier.width(12.dp))
                        Text("Scanning…", color = Color.White)
                    }
                }
                biosScanError.value != null -> {
                    Text(biosScanError.value!!, color = Color(0xFFFF6B6B))
                }
                scannedBioses.isNotEmpty() -> {
                    // 2-column grid — landscape layout has plenty of room and
                    // a single column wastes most of the screen.
                    LazyVerticalGrid(
                        columns = GridCells.Fixed(2),
                        modifier = Modifier.fillMaxSize(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp),
                    ) {
                        itemsIndexed(scannedBioses) { idx, bios ->
                            val selected = selectedBiosIdx.value == idx
                            BiosRow(
                                info = bios.info,
                                displayName = bios.displayName,
                                selected = selected,
                                onClick = {
                                    selectedBiosIdx.value = idx
                                    refreshAllowNext()
                                },
                            )
                        }
                    }
                }
                // Fallback: no folder URI to scan AND no scan running. If a
                // BIOS is already configured, render it as a single tile in
                // the same 2-column grid so it visually matches the picker
                // layout for users re-entering setup.
                configuredPath != null && configuredBiosInfo.value != null -> {
                    LazyVerticalGrid(
                        columns = GridCells.Fixed(2),
                        modifier = Modifier.fillMaxSize(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp),
                    ) {
                        item {
                            BiosRow(
                                info = configuredBiosInfo.value!!,
                                displayName = File(configuredPath).name,
                                selected = true,
                                onClick = { /* no-op — already configured */ },
                            )
                        }
                    }
                }
                else -> {
                    Text(
                        "No BIOS folder selected yet — use the Pick BIOS Folder button below.",
                        color = Color.LightGray,
                    )
                }
            }
        }
    }

    @Composable
    private fun BiosRow(
        info: BiosInfo,
        displayName: String,
        selected: Boolean,
        onClick: () -> Unit,
    ) {
        val bg = if (selected) Colors.pasx2_blue.copy(alpha = 0.35f) else Color(0xFF333333)
        val border = if (selected) Colors.pasx2_blue else Color.Transparent
        Row(
            Modifier
                .fillMaxWidth()
                .padding(vertical = 4.dp)
                .clip(RoundedCornerShape(8.dp))
                .background(bg)
                .border(2.dp, border, RoundedCornerShape(8.dp))
                .clickable(onClick = onClick)
                .padding(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(info.regionFlag, fontSize = 28.sp)
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        info.zone.ifBlank { "Unknown" },
                        color = Color.White, fontSize = 18.sp, fontWeight = FontWeight.Bold,
                    )
                    Spacer(Modifier.width(8.dp))
                    Text(info.versionString, color = Color(0xFFAACCFF), fontSize = 16.sp)
                }
                Spacer(Modifier.height(2.dp))
                Text(displayName, color = Color.LightGray, fontSize = 12.sp)
                Spacer(Modifier.height(2.dp))
                Text(info.description, color = Color(0xFFCCCCCC), fontSize = 11.sp)
            }
        }
    }

    /** System folder page — confirmation of the selected folder. Picker
     *  lives in the nav row. The folder is where emucore will look for
     *  bios/, memcards/, savestates/, etc.; before this page existed,
     *  emucore defaulted to getExternalFilesDir(null). */
    @Composable
    private fun SetupSystemDirContent() {
        Column(Modifier.fillMaxSize()) {
            Text(
                "Pick the folder ARMSX2 should use for system files (memory cards, save states, configs). " +
                "Defaults to Android/data/com.armsx2/files when unset.",
                fontSize = 14.sp, color = Color.LightGray,
                modifier = Modifier.padding(bottom = 12.dp),
            )

            // Validation error banner — surfaces the scoped-storage write
            // rejection so the user knows why Next refused. The grant
            // intent has already been launched at this point on
            // Android 11+; the user just needs to flip the toggle and
            // re-tap Next.
            val err = systemDirError.value
            if (err != null) {
                Row(
                    Modifier.fillMaxWidth()
                        .clip(RoundedCornerShape(8.dp))
                        .background(Color(0xFF5A1A1A))
                        .padding(12.dp)
                        .padding(bottom = 0.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("⚠", fontSize = 22.sp, color = Color(0xFFFF6B6B))
                    Spacer(Modifier.width(8.dp))
                    Text(err, color = Color.White, fontSize = 13.sp,
                        modifier = Modifier.weight(1f))
                }
                Spacer(Modifier.height(12.dp))
            }

            val display = systemDirDisplay.value
            if (display != null && !systemDirUseDefault.value) {
                Row(
                    Modifier.fillMaxWidth()
                        .clip(RoundedCornerShape(8.dp))
                        .background(Color(0xFF333333))
                        .padding(12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("📁", fontSize = 24.sp)
                    Spacer(Modifier.width(8.dp))
                    Column(Modifier.weight(1f)) {
                        Text("Selected:", color = Color.LightGray, fontSize = 12.sp)
                        Text(display, color = Color.White, fontSize = 14.sp)
                    }
                }
            } else if (systemDirUseDefault.value) {
                Row(
                    Modifier.fillMaxWidth()
                        .clip(RoundedCornerShape(8.dp))
                        .background(Color(0xFF1F3A1F))
                        .padding(12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("✅", fontSize = 22.sp)
                    Spacer(Modifier.width(8.dp))
                    Column(Modifier.weight(1f)) {
                        Text("Using App-Private Folder", color = Color.White, fontSize = 13.sp,
                            fontWeight = FontWeight.Bold)
                        Text("Android/data/com.armsx2/files",
                            color = Color.LightGray, fontSize = 11.sp)
                    }
                }
            } else {
                Text("No system folder selected yet — use the Pick System Folder button below.",
                    color = Color.LightGray)
            }

            Spacer(Modifier.height(12.dp))

            // Escape hatch — guaranteed-writable app-private fallback.
            // Clicking this clears any picked SAF URI + sets the
            // use-default sentinel so refreshAllowNext + finishSystemDirStep
            // know to skip the SAF write probe on Next.
            Button(
                onClick = {
                    systemDirUseDefault.value = true
                    systemDirUri.value = null
                    systemDirDisplay.value = null
                    systemDirError.value = null
                    refreshAllowNext()
                },
                colors = ps2Colors(),
                shape = RoundedCornerShape(8.dp),
            ) {
                Text(
                    if (systemDirUseDefault.value)
                        "✓ Using App-Private Folder"
                    else
                        "Use App-Private Folder (default, always writable)",
                )
            }
        }
    }

    /** ROMs page content — list of selected folders, each with a remove
     *  button. The Pick / Add button lives in the nav row at the bottom
     *  (midButtonLabel returns "Add Another Folder" when the list is
     *  non-empty, "Pick ROMs Folder" when empty). */
    @Composable
    private fun SetupRomsDirContent() {
        Column(Modifier.fillMaxSize()) {
            Text(
                "Pick one or more folders where you keep your PS2 (.iso / .chd / etc.) and PS1 (.bin / .iso) ROM dumps. " +
                "ARMSX2 will list games from every folder you add.",
                fontSize = 14.sp, color = Color.LightGray,
                modifier = Modifier.padding(bottom = 12.dp),
            )

            if (romsDirsState.isEmpty()) {
                Text(
                    "No ROMs folders yet — use the Pick ROMs Folder button below to add one.",
                    color = Color.LightGray,
                )
            } else {
                Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    for (uri in romsDirsState.toList()) {
                        RomsDirRow(
                            uri = uri,
                            onRemove = {
                                romsDirsState.remove(uri)
                                refreshAllowNext()
                            },
                        )
                    }
                }
            }
        }
    }

    @Composable
    private fun RomsDirRow(uri: Uri, onRemove: () -> Unit) {
        val display = uri.lastPathSegment ?: uri.toString()
        Row(
            Modifier.fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                .background(Color(0xFF333333))
                .padding(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("📁", fontSize = 22.sp)
            Spacer(Modifier.width(10.dp))
            Column(Modifier.weight(1f)) {
                Text(display, color = Color.White, fontSize = 14.sp,
                    fontWeight = FontWeight.Bold)
                Text(uri.toString(), color = Color(0xFFAAAAAA), fontSize = 10.sp,
                    maxLines = 1)
            }
            Spacer(Modifier.width(8.dp))
            // Tap-to-remove. Tinted PS2-blue-ish so it's obviously
            // interactive without dominating the row.
            Box(
                Modifier
                    .clip(RoundedCornerShape(6.dp))
                    .background(Color(0xFF5A1A1A))
                    .clickable(onClick = onRemove)
                    .padding(horizontal = 10.dp, vertical = 6.dp),
            ) {
                Text("Remove", color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.Bold)
            }
        }
    }

    /**
     * Enumerate `treeUri` via DocumentFile, probe each candidate file via
     * NativeApp.getBiosInfoFromFd, populate scannedBioses with the valid
     * hits. Runs on Main.invoke's background dispatcher. On success the
     * URI is persisted to prefs as `biosDir` and the configured BIOS (if
     * any) is pre-selected in the list by filename match.
     */
    private fun scanBiosDirectory(context: Context, treeUri: Uri) {
        biosScanning.value = true
        biosScanError.value = null
        scannedBioses.clear()
        selectedBiosIdx.value = null
        refreshAllowNext()
        lastScannedDir.value = treeUri

        Main.invoke {
            try {
                val tree = DocumentFile.fromTreeUri(context, treeUri)
                val files = tree?.listFiles() ?: emptyArray()
                for (f in files) {
                    if (!f.isFile) continue
                    val len = f.length()
                    // PS2 BIOS images are 4MB; allow up to 8.5MB to cover
                    // hash-suffixed dumps that slip into the dir. SAF can
                    // return 0/-1 for unknown size — fall through and let
                    // the BIOS probe itself reject non-images.
                    if (len > 0L && (len < 4_000_000L || len > 8_500_000L)) continue

                    val pfd: ParcelFileDescriptor? = try {
                        context.contentResolver.openFileDescriptor(f.uri, "r")
                    } catch (_: Exception) { null }
                    if (pfd == null) continue
                    val fd = pfd.detachFd()
                    val info = NativeApp.getBiosInfoFromFd(fd)
                    if (info != null) {
                        val name = f.name ?: "unknown.bin"
                        scannedBioses.add(ScannedBios(f.uri, name, info))
                    }
                }

                // Remember the folder for next-launch rescan and pre-select
                // the configured BIOS so the user sees their existing choice
                // highlighted on re-entry. Only persist on a non-empty scan
                // — if the folder is genuinely empty / no BIOSes detected
                // we don't want to overwrite a known-good URI with a junk
                // one the user picked by mistake.
                if (scannedBioses.isNotEmpty()) {
                    biosDirUri.value = treeUri
                    Main.biosDir.value = treeUri.toString()
                    Main.prefs.edit().putString("biosDir", treeUri.toString()).apply()

                    val configuredName = Main.bios.value?.let { File(it).name }
                    if (configuredName != null) {
                        val matchIdx = scannedBioses.indexOfFirst { it.displayName == configuredName }
                        if (matchIdx >= 0) {
                            selectedBiosIdx.value = matchIdx
                            refreshAllowNext()
                        }
                    }
                }
            } catch (e: Exception) {
                biosScanError.value = "Scan failed: ${e.message}"
            } finally {
                biosScanning.value = false
            }
        }
    }
}
