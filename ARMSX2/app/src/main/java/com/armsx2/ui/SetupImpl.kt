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
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
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

    // -------- ROMs dir setup state --------
    private val romsDirUri = mutableStateOf<Uri?>(null)
    private val romsDirDisplay = mutableStateOf<String?>(null)

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
        val uri = systemDirUri.value
        // Re-entry path: keep existing pref if user didn't repick. The
        // persistable grant from a prior session survives across process
        // restarts so we don't need to re-take it.
        if (uri == null) return Main.systemDir.value
        try {
            context.contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        } catch (_: SecurityException) { /* already persisted, or revoked */ }
        Main.systemDir.value = uri.toString()
        Main.prefs.edit().putString("systemDir", uri.toString()).apply()
        return uri.toString()
    }

    private fun finishRomsStep(context: Context): String? {
        val uri = romsDirUri.value
        // Re-entry from Settings cog: keep the previous URI if no new one
        // was picked. The persistable-permission grant survives across
        // process restarts, so we don't need to re-take it.
        if (uri == null) return Main.romsDir.value

        try {
            context.contentResolver.takePersistableUriPermission(
                uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (_: SecurityException) { /* already persisted, or revoked */ }
        Main.romsDir.value = uri.toString()
        Main.prefs.edit().putString("roms", uri.toString()).apply()
        return uri.toString()
    }

    private fun refreshAllowNext() {
        allowNext.value = when (setupState.value) {
            0 -> true
            // System dir page — Next when the user has a fresh URI selected
            // OR an existing pref to keep (re-entry path).
            1 -> systemDirUri.value != null || Main.systemDir.value != null
            // BIOS page — Next when a row is picked, or (fallback for revoked
            // / missing biosDir) an already-configured BIOS we're keeping.
            2 -> selectedBiosIdx.value != null ||
                 (biosDirUri.value == null && Main.bios.value != null)
            // ROMs page.
            3 -> romsDirUri.value != null
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

        val existingRoms = Main.romsDir.value
        if (existingRoms != null) {
            try {
                val uri = Uri.parse(existingRoms)
                romsDirUri.value = uri
                romsDirDisplay.value = uri.lastPathSegment ?: existingRoms
            } catch (_: Exception) {
                romsDirUri.value = null
                romsDirDisplay.value = null
            }
        } else {
            romsDirUri.value = null
            romsDirDisplay.value = null
        }
    }

    /** Page heading shown in the upper-left of the title row. */
    private fun pageTitle(): String = when (setupState.value) {
        0 -> "Welcome"
        1 -> "Select system folder"
        2 -> "Select your BIOS"
        3 -> "Select ROMs folder"
        else -> ""
    }

    /** Label for the page-local action button (in the nav row). null = no button. */
    private fun midButtonLabel(): String? = when (setupState.value) {
        1 -> if (systemDirUri.value == null) "Pick System Folder" else "Pick a different folder"
        // Use the URI presence (not the in-memory list) so the label says
        // "Pick a different folder" immediately on re-entry when we already
        // have a remembered biosDir, even before the auto-rescan finishes.
        2 -> if (biosDirUri.value == null) "Pick BIOS Folder" else "Pick a different folder"
        3 -> if (romsDirUri.value == null) "Pick ROMs Folder" else "Pick a different folder"
        else -> null
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
                romsDirUri.value = treeUri
                romsDirDisplay.value = treeUri.lastPathSegment ?: treeUri.toString()
                refreshAllowNext()
            }
        }

        Box(Modifier.fillMaxSize().background(Colors.surface.value)) {
            Column(Modifier.fillMaxSize()) {

                // Title row — page heading on the left, ARMSX2 logo on the right.
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
                    Text("ARMSX2", color = Color.White, fontSize = 16.sp)
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
                        1 -> SetupSystemDirContent()
                        2 -> SetupBiosContent()
                        3 -> SetupRomsDirContent()
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
                                    1 -> systemLauncher.launch(null)
                                    2 -> biosLauncher.launch(null)
                                    3 -> romsLauncher.launch(null)
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
                                    if (finishSystemDirStep(context) != null) {
                                        setupState.value = 2
                                        allowPrev.value = true
                                        refreshAllowNext()
                                    }
                                }
                                2 -> {
                                    if (finishBiosStep(context) != null) {
                                        setupState.value = 3
                                        allowPrev.value = true
                                        refreshAllowNext()
                                    }
                                }
                                3 -> {
                                    if (finishRomsStep(context) != null) {
                                        setupState.value = 4
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
                        Text(if (setupState.value == 3) "Finish" else "Next")
                    }
                }
            }
        }
    }

    @Composable
    fun Welcome() {
        Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.Center) {
            Text("Welcome to ARMSX2 setup!", Modifier.align(Alignment.CenterHorizontally),
                fontSize = 28.sp, color = Color.White, fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(8.dp))
            Text("Hit Next to get started", Modifier.align(Alignment.CenterHorizontally),
                fontSize = 14.sp, color = Color.LightGray)

            Spacer(Modifier.height(28.dp))

            // Renderer selector — segmented OpenGL / Vulkan toggle.
            Text(
                "Renderer backend",
                Modifier.align(Alignment.CenterHorizontally),
                color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.Bold,
            )
            Spacer(Modifier.height(6.dp))
            Row(
                Modifier.align(Alignment.CenterHorizontally),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                RendererOption("OpenGL", "opengl")
                RendererOption("Vulkan", "vulkan")
            }

            Spacer(Modifier.height(20.dp))

            // Upscale dropdown — 1x..5x. Each option shows the resulting
            // internal resolution so the user can gauge the perf cost.
            Text(
                "Upscale multiplier",
                Modifier.align(Alignment.CenterHorizontally),
                color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.Bold,
            )
            Spacer(Modifier.height(6.dp))
            UpscaleDropdown()
        }
    }

    /** PS2-blue squared-rounded option for the renderer choice. */
    @Composable
    private fun RendererOption(label: String, value: String) {
        val selected = Main.renderer.value == value
        Button(
            onClick = {
                Main.renderer.value = value
                Main.prefs.edit().putString("renderer", value).apply()
            },
            shape = RoundedCornerShape(8.dp),
            colors = ButtonDefaults.buttonColors(
                containerColor = if (selected) Colors.pasx2_blue else Color(0xFF333333),
                contentColor = Color.White,
            ),
            contentPadding = PaddingValues(horizontal = 14.dp, vertical = 8.dp),
        ) {
            Text(label)
        }
    }

    /** Anchor button + DropdownMenu listing 1x..5x upscale options with
     *  their resulting internal resolution. PS2 native is 640×448 NTSC.
     *  The button + menu are wrapped in a content-sized Box so the menu
     *  anchors directly under the button instead of the parent's far-left
     *  edge. */
    @Composable
    private fun UpscaleDropdown() {
        val expanded = remember { mutableStateOf(false) }
        val options = listOf(1, 2, 3, 4, 5)
        Row(
            Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.Center,
        ) {
            // Inner content-sized box anchors the DropdownMenu under the
            // button itself, not the outer fillMaxWidth row.
            Box {
                Button(
                    onClick = { expanded.value = true },
                    shape = RoundedCornerShape(8.dp),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = Colors.pasx2_blue,
                        contentColor = Color.White,
                    ),
                    contentPadding = PaddingValues(horizontal = 14.dp, vertical = 8.dp),
                ) {
                    Text(upscaleLabel(Main.upscale.value))
                }
                DropdownMenu(
                    expanded = expanded.value,
                    onDismissRequest = { expanded.value = false },
                    // Dark surface to match the rest of the wizard. The
                    // default Material surface is near-white and clashes
                    // with the dark wizard background.
                    modifier = Modifier.background(Color(0xFF1F1F1F)),
                ) {
                    for (m in options) {
                        DropdownMenuItem(
                            text = {
                                Text(
                                    upscaleLabel(m),
                                    color = Color.White,
                                    fontSize = 14.sp,
                                )
                            },
                            onClick = {
                                Main.upscale.value = m
                                Main.prefs.edit().putInt("upscale", m).apply()
                                expanded.value = false
                            },
                        )
                    }
                }
            }
        }
    }

    private fun upscaleLabel(mult: Int): String {
        // PS2 NTSC native = 640×448. Multiplier scales both axes.
        val w = 640 * mult
        val h = 448 * mult
        return "${mult}x  ${w}×${h}"
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
                    LazyColumn(Modifier.fillMaxSize()) {
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
                // BIOS is already configured, render it as a single read-
                // only row so the user knows it's preserved.
                configuredPath != null && configuredBiosInfo.value != null -> {
                    LazyColumn(Modifier.fillMaxSize()) {
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

            val display = systemDirDisplay.value
            if (display != null) {
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
            } else {
                Text("No system folder selected yet — use the Pick System Folder button below.",
                    color = Color.LightGray)
            }
        }
    }

    /** ROMs page content — selected-folder confirmation. Picker is in the nav row. */
    @Composable
    private fun SetupRomsDirContent() {
        Column(Modifier.fillMaxSize()) {
            Text(
                "Pick the folder where you keep your PS2 ROM dumps (.iso / .chd / .gz / etc.). ARMSX2 will list games from this folder.",
                fontSize = 14.sp, color = Color.LightGray,
                modifier = Modifier.padding(bottom = 12.dp),
            )

            val display = romsDirDisplay.value
            if (display != null) {
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
            } else {
                Text("No ROMs folder selected yet — use the Pick ROMs Folder button below.",
                    color = Color.LightGray)
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
