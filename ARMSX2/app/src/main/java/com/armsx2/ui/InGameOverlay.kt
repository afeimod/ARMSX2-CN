package com.armsx2.ui

import android.content.Intent
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.interaction.MutableInteractionSource
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
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.SubcomposeAsyncImage
import coil.request.ImageRequest
import com.armsx2.EmuState
import com.armsx2.Main
import com.armsx2.R
import com.armsx2.RenderMode
import com.armsx2.config.ConfigStore
import com.armsx2.config.Settings
import com.armsx2.ui.settings.PerformanceTab
import com.armsx2.ui.settings.RendererTab
import kr.co.iefriends.pcsx2.NativeApp

/**
 * In-game pause overlay. Layout matches the look of pcsx2-qt's in-game
 * fullscreen menu:
 *
 *   ┌──────────────────────────────────────────────────────┐
 *   │ Game Title                          ARMSX2  [logo]   │
 *   │ SLUS-12345 · ★★★★☆                  v2.7.304-3       │
 *   │                                                       │
 *   │              (game surface visible through            │
 *   │               the partial-alpha backdrop)             │
 *   │                                                       │
 *   │ Resume Game                                           │
 *   │ Show Toolbar                                          │
 *   │ Save State                                            │
 *   │ ...                                                   │
 *   │ Close Game                                            │
 *   └──────────────────────────────────────────────────────┘
 *
 * Trigger: long-press on the game surface while RUNNING (Main.kt's
 * detectTapGestures handler). Open auto-pauses; close paths inside the
 * overlay decide whether to resume.
 *
 * Mirrors PCSX2 ImGui FullscreenUI's DrawPauseMenu (FullscreenUI.cpp:1549+)
 * minus features that aren't wired on the Android port (achievements,
 * per-game props, screenshot snapshot).
 */
object InGameOverlay {
    private sealed class State {
        data object Root : State()
        data object SaveStateSlots : State()
        data object LoadStateSlots : State()
        data object ExitConfirm : State()
        data object ResetConfirm : State()
    }

    private val state = mutableStateOf<State>(State.Root)

    // Tab selection inside the Root state. Tabs are config groups —
    // PlayingNow holds the existing pause-menu options (Resume / Save
    // State / Change Disc / Reset / Close / etc), Performance and
    // Renderer host the speedhack + GS toggles backed by ConfigStore.
    private enum class Tab(val label: String) {
        PlayingNow("Playing Now"),
        Performance("Performance"),
        Renderer("Renderer"),
    }
    private val currentTab = mutableStateOf(Tab.PlayingNow)

    // Live Settings state shared across the Performance + Renderer tabs.
    // Hydrated from ConfigStore on every overlay open so we pick up any
    // out-of-band edits (e.g. from a future global Settings screen).
    private val settingsState = mutableStateOf(Settings())

    // Locally tracked frame-limit toggle. 0 = Nominal (60fps cap),
    // 3 = Unlimited. Matches LimiterModeType / SpeedhackButton wiring.
    private val frameLimitOn = mutableStateOf(true)

    // Renderer cycle: matches RenderModeButton's OPENGL ↔ VULKAN_SW path.
    private val currentRenderer = mutableStateOf(RenderMode.OPENGL)

    // Tracks whether THIS overlay is the one that paused the VM. If the
    // user already had the VM paused (e.g. via toolbar) before opening
    // the overlay, closing the overlay shouldn't auto-resume.
    private var pausedByOverlay = false

    /**
     * Build version string sourced from `BuildVersion.cpp`:
     *   GitTagHi.GitTagMid.GitTagLo-ARMSX2Build
     * Hardcoded here because there's no JNI accessor today; bump in
     * lockstep with the C++ side when those constants change.
     */
    private const val VERSION_STRING = "v2.7.304-3"

    /** Open the overlay. Pauses the VM. Safe to call when already open. */
    fun open() {
        if (WindowImpl.overlayVisible.value) return
        pausedByOverlay = (Main.eState.value == EmuState.RUNNING)
        if (pausedByOverlay) Main.pause()
        state.value = State.Root
        currentTab.value = Tab.PlayingNow
        // Re-hydrate settings from disk in case anything edited out-of-band.
        settingsState.value = ConfigStore.loadGlobal()
        WindowImpl.overlayVisible.value = true
    }

    private fun closeAndResume() {
        WindowImpl.overlayVisible.value = false
        state.value = State.Root
        if (pausedByOverlay && Main.eState.value == EmuState.PAUSED) Main.resume()
        pausedByOverlay = false
    }

    private fun closeKeepingState() {
        WindowImpl.overlayVisible.value = false
        state.value = State.Root
        pausedByOverlay = false
    }

    @Composable
    fun Render() {
        // Hardware back: descends submenu first, then dismisses (resumes).
        BackHandler(enabled = WindowImpl.overlayVisible.value) {
            when (state.value) {
                is State.Root -> closeAndResume()
                else -> state.value = State.Root
            }
        }

        // Backdrop swallows taps so the running surface beneath doesn't
        // pick up the press as a button event. Tap-outside on Root acts
        // as Resume Game.
        val backdropInteraction = remember { MutableInteractionSource() }
        Box(
            Modifier
                .fillMaxSize()
                .background(Color(0xFF101010).copy(alpha = 0.5f))
                .clickable(
                    indication = null,
                    interactionSource = backdropInteraction,
                    onClick = { if (state.value is State.Root) closeAndResume() }
                ),
        ) {
            // Top-left: game info (title, serial, compatibility stars).
            GameInfoHeader(
                modifier = Modifier
                    .align(Alignment.TopStart)
                    .padding(20.dp),
            )

            // Top-right: ARMSX2 branding + version. Mirrors the SetupImpl
            // title row's right-aligned "ARMSX2 [logo]".
            BrandHeader(
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .padding(20.dp),
            )

            // Bottom-left: menu items (Root) or submenu content. Sized to
            // fit without scrolling — see comment on RootMenu.
            Box(
                Modifier
                    .align(Alignment.BottomStart)
                    .fillMaxHeight(0.75f)
                    .width(360.dp)
                    .padding(start = 20.dp, bottom = 20.dp, top = 20.dp),
                contentAlignment = Alignment.BottomStart,
            ) {
                when (state.value) {
                    is State.Root -> RootTabs()
                    is State.SaveStateSlots -> SaveStatePicker.Render(
                        mode = SaveStatePicker.Mode.Save,
                        onDone = { closeAndResume() },
                        onBack = { state.value = State.Root },
                    )
                    is State.LoadStateSlots -> SaveStatePicker.Render(
                        mode = SaveStatePicker.Mode.Load,
                        onDone = { closeAndResume() },
                        onBack = { state.value = State.Root },
                    )
                    is State.ExitConfirm -> ExitConfirm()
                    is State.ResetConfirm -> ResetConfirm()
                }
            }
        }
    }

    @Composable
    private fun GameInfoHeader(modifier: Modifier = Modifier) {
        // Prefer the cached GameInfo from Main.currentGame — it has the
        // pre-resolved compat (computed at library scan time), the cover
        // URL, and the container extension. Fallback to NativeApp.getPause*
        // for paths that lack a GameInfo (Change Disc file picker, BIOS).
        val cached = Main.currentGame.value
        val title = cached?.title?.takeIf { it.isNotEmpty() }
            ?: NativeApp.getPauseGameTitle()?.takeIf { it.isNotEmpty() }
            ?: "PS2 BIOS"
        val serial = cached?.serial
            ?: NativeApp.getPauseGameSerial()?.takeIf { it.isNotEmpty() }
        val compatStars = when {
            cached != null -> cached.compatibility
            serial != null -> (NativeApp.getCompatibilityForSerial(serial) - 1).coerceIn(0, 5)
            else -> 0
        }
        val coverUrl = cached?.coverUrl
        val extension = cached?.extension?.takeIf { it.isNotEmpty() }

        Row(modifier = modifier, verticalAlignment = Alignment.Top) {
            // Cover thumbnail to the left, matching the library card's
            // 0.7 aspect ratio. 72dp tall fits next to a 22sp title +
            // 13sp serial line without crowding.
            Box(
                Modifier
                    .height(72.dp)
                    .aspectRatio(0.7f)
                    .clip(RoundedCornerShape(4.dp))
                    .background(Color(0xFF1B1A1A).copy(alpha = 0.5f)),
                contentAlignment = Alignment.Center,
            ) {
                val context = LocalContext.current
                if (coverUrl != null) {
                    SubcomposeAsyncImage(
                        model = ImageRequest.Builder(context).data(coverUrl).crossfade(true).build(),
                        contentDescription = "$title cover",
                        contentScale = ContentScale.Crop,
                        modifier = Modifier.fillMaxSize(),
                        loading = { /* dim background shows through */ },
                        error = { Text("📀", color = Color(0xFF3F3F3F), fontSize = 28.sp) },
                    )
                } else {
                    Text("📀", color = Color(0xFF3F3F3F), fontSize = 28.sp)
                }
            }
            Spacer(Modifier.width(12.dp))

            Column {
                Text(title, color = Color.White, fontSize = 22.sp, fontWeight = FontWeight.Bold)
                Spacer(Modifier.height(4.dp))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        serial ?: "No disc",
                        color = if (serial != null) Color(0xFFAACCFF) else Color(0xFF808080),
                        fontSize = 13.sp,
                    )
                    if (serial != null) {
                        Spacer(Modifier.width(10.dp))
                        CompatStars(compatStars)
                    }
                    if (extension != null) {
                        Spacer(Modifier.width(8.dp))
                        ExtensionBadge(extension)
                    }
                }
            }
        }
    }

    @Composable
    private fun CompatStars(filled: Int) {
        Row {
            repeat(5) { i ->
                val on = i < filled
                Text(
                    if (on) "★" else "☆",
                    color = if (on) Color(0xFFFFD33A) else Color(0xFF555555),
                    fontSize = 13.sp,
                )
            }
        }
    }

    /** PS2-blue rounded chip showing the container format (ISO / CHD /
     *  BIN / etc.). Mirrors GamesList's ExtensionBadge for parity with
     *  the library card chrome. */
    @Composable
    private fun ExtensionBadge(ext: String) {
        Box(
            Modifier
                .clip(RoundedCornerShape(4.dp))
                .background(Colors.pasx2_blue)
                .padding(horizontal = 5.dp, vertical = 1.dp),
        ) {
            Text(
                ext,
                color = Color.White,
                fontSize = 9.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
            )
        }
    }

    @Composable
    private fun BrandHeader(modifier: Modifier = Modifier) {
        // Brand text column (ARMSX2 + version stacked, version centered
        // under the ARMSX2 text only) sits beside the logo. The version
        // line is horizontally centered against the ARMSX2 word's
        // intrinsic width — not against the whole row including the
        // icon — so it reads as a label belonging to the wordmark.
        Row(
            modifier = modifier,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text("ARMSX2", color = Color.White, fontSize = 18.sp, fontWeight = FontWeight.Bold)
                Text(VERSION_STRING, color = Color(0xFF888888), fontSize = 11.sp)
            }
            Spacer(Modifier.width(8.dp))
            Image(
                painter = painterResource(id = R.drawable.savetowerforeground),
                contentDescription = null,
                modifier = Modifier.size(32.dp),
            )
        }
    }

    /** Root content — tab strip + active tab body. Tabs are config groups:
     *  Playing Now hosts the pause-menu actions (Resume / Save State /
     *  Change Disc / Reset / Close), Performance + Renderer host the
     *  ConfigStore-backed settings widgets.
     *
     *  fillMaxSize so the verticalScroll inside Performance/Renderer tabs
     *  has a bounded parent and can actually scroll on overflow. The parent
     *  Box's contentAlignment is moot once we fill — content flows top-down
     *  from the box's top edge (which is already at ~25% of screen height). */
    @Composable
    private fun RootTabs() {
        Column(modifier = Modifier.fillMaxSize()) {
            TabStrip()
            Spacer(Modifier.height(6.dp))
            when (currentTab.value) {
                Tab.PlayingNow -> PlayingNowTab()
                Tab.Performance -> PerformanceTab(settingsState)
                Tab.Renderer -> RendererTab(settingsState)
            }
        }
    }

    /** Horizontal tab chip strip. Active tab gets PS2-blue underline +
     *  brighter text; inactive tabs are dim. Tappable across the whole
     *  chip area. */
    @Composable
    private fun TabStrip() {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Tab.values().forEach { tab ->
                val active = currentTab.value == tab
                Column(
                    modifier = Modifier
                        .weight(1f)
                        .clickable { currentTab.value = tab }
                        .padding(vertical = 4.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Text(
                        tab.label,
                        color = if (active) Color.White else Color(0xFF888888),
                        fontSize = 12.sp,
                        fontWeight = FontWeight.Bold,
                    )
                    Spacer(Modifier.height(3.dp))
                    Box(
                        Modifier
                            .fillMaxWidth()
                            .height(2.dp)
                            .background(
                                if (active) Colors.pasx2_blue else Color.Transparent
                            ),
                    )
                }
            }
        }
    }

    /** Playing Now — the original pause-menu actions, rendered as a
     *  fixed Column of compact rows + fading dividers. No scroll needed
     *  for these 10 items. */
    @Composable
    private fun PlayingNowTab() {
        Column(modifier = Modifier.fillMaxWidth()) {
            MenuRow("Resume Game") { closeAndResume() }
            MenuDivider()
            MenuRow("Show Toolbar") {
                WindowImpl.toolbarVisible.value = true
                closeKeepingState()
            }
            MenuDivider()
            MenuRow("Save State") { state.value = State.SaveStateSlots }
            MenuDivider()
            MenuRow("Load State") { state.value = State.LoadStateSlots }
            MenuDivider()
            MenuRow("Change Disc") {
                val intent = Intent(Intent.ACTION_GET_CONTENT)
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, false)
                intent.setType("*/*")
                Main.instance?.openFileAction?.launch(intent)
                closeKeepingState()
            }
            MenuDivider()
            MenuRow("Open Game Library") {
                if (Main.eState.value == EmuState.PAUSED) Main.resume()
                WindowImpl.showLibrary.value = true
                closeKeepingState()
            }
            MenuDivider()
            val rendererLabel = when (currentRenderer.value) {
                RenderMode.OPENGL -> "Renderer: OpenGL HW"
                RenderMode.VULKAN_SW -> "Renderer: Software"
            }
            MenuRow(rendererLabel) {
                when (currentRenderer.value) {
                    RenderMode.OPENGL -> {
                        currentRenderer.value = RenderMode.VULKAN_SW
                        Main.renderSoftware()
                    }
                    RenderMode.VULKAN_SW -> {
                        currentRenderer.value = RenderMode.OPENGL
                        Main.renderOpenGL()
                    }
                }
            }
            MenuDivider()
            MenuRow(if (frameLimitOn.value) "Frame Limit: On" else "Frame Limit: Off") {
                frameLimitOn.value = !frameLimitOn.value
                NativeApp.speedhackLimitermode(if (frameLimitOn.value) 0 else 3)
            }
            MenuDivider()
            MenuRow("Reset System") { state.value = State.ResetConfirm }
            MenuDivider()
            MenuRow("Close Game", danger = true) { state.value = State.ExitConfirm }
        }
    }

    /**
     * Thin horizontal divider with a left-anchored fade — opaque at the
     * left edge (where the row label starts) and fading to transparent
     * at the right edge. The row "aura" backgrounds use a fainter
     * version of this same gradient so the dividers and rows visually
     * tie together.
     */
    @Composable
    private fun MenuDivider() {
        Box(
            Modifier
                .fillMaxWidth()
                .height(1.dp)
                .background(
                    Brush.horizontalGradient(
                        listOf(
                            Color.White.copy(alpha = 0.35f),
                            Color.Transparent,
                        )
                    )
                ),
        )
    }

    @Composable
    private fun ExitConfirm() {
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(
                "Close current game?",
                color = Color.White,
                fontSize = 14.sp,
                modifier = Modifier.padding(bottom = 6.dp),
            )
            MenuRow("Save State And Exit") {
                NativeApp.saveStateToSlot(0)
                Main.stop()
                closeKeepingState()
            }
            MenuRow("Exit Without Saving", danger = true) {
                Main.stop()
                closeKeepingState()
            }
            MenuRow("Back") { state.value = State.Root }
        }
    }

    @Composable
    private fun ResetConfirm() {
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(
                "Reset the system? Unsaved progress will be lost.",
                color = Color.White,
                fontSize = 14.sp,
                modifier = Modifier.padding(bottom = 6.dp),
            )
            MenuRow("Yes, Reset", danger = true) {
                Main.restart()
                closeKeepingState()
            }
            MenuRow("Back") { state.value = State.Root }
        }
    }

    /** Single menu row. Compact (24dp) so all 10 root items fit without
     *  scrolling on phone landscape. Background is a left-anchored
     *  alpha gradient that "auras" the text and matches the divider
     *  fade direction. Danger variant tints text red for destructive
     *  actions (Close Game / Exit Without Saving). */
    @Composable
    private fun MenuRow(
        label: String,
        danger: Boolean = false,
        onClick: () -> Unit,
    ) {
        val textColor = if (danger) Color(0xFFFF6B6B) else Color.White
        val auraStart = if (danger)
            Color(0xFFFF6B6B).copy(alpha = 0.10f)
        else
            Color.White.copy(alpha = 0.06f)
        Box(
            Modifier
                .fillMaxWidth()
                .height(24.dp)
                .background(
                    Brush.horizontalGradient(
                        listOf(
                            auraStart,
                            Color.Transparent,
                        )
                    )
                )
                .clickable(onClick = onClick)
                .padding(horizontal = 6.dp),
            contentAlignment = Alignment.CenterStart,
        ) {
            Text(label, color = textColor, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
        }
    }
}
