package com.armsx2.ui

import android.content.Intent
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.displayCutoutPadding
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.Text
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.vector.ImageVector
import compose.icons.LineAwesomeIcons
import compose.icons.lineawesomeicons.CompactDiscSolid
import compose.icons.lineawesomeicons.CubeSolid
import compose.icons.lineawesomeicons.EyeSlashSolid
import compose.icons.lineawesomeicons.EyeSolid
import compose.icons.lineawesomeicons.FolderOpenSolid
import compose.icons.lineawesomeicons.PlaySolid
import compose.icons.lineawesomeicons.PowerOffSolid
import compose.icons.lineawesomeicons.RedoAltSolid
import compose.icons.lineawesomeicons.SaveSolid
import compose.icons.lineawesomeicons.TachometerAltSolid
import compose.icons.lineawesomeicons.ThLargeSolid
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.keyframes
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.SubcomposeAsyncImage
import coil.request.ImageRequest
import com.armsx2.EmuState
import com.armsx2.Main
import com.armsx2.R
import com.armsx2.config.ConfigStore
import com.armsx2.config.Settings
import com.armsx2.ui.settings.PerformanceTab
import com.armsx2.ui.settings.RecompilerTab
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
        data object AchievementsLogin : State()
        data object HardcoreEnableConfirm : State()
        data object HardcoreSaveStateBlocked : State()
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
        Recompiler("Recompiler"),
    }
    private val currentTab = mutableStateOf(Tab.PlayingNow)

    // Live Settings state shared across the Performance + Renderer tabs.
    // Hydrated from ConfigStore on every overlay open so we pick up any
    // out-of-band edits (e.g. from a future global Settings screen).
    private val settingsState = mutableStateOf(Settings())

    // Polled from NativeApp.isHardcoreMode while the overlay is open. Drives
    // the Save/Load state row dimming and the AchievementsPanel button
    // colour. Updates on every overlay open and every AchievementsPanel
    // poll (which already runs every 4s) — see Render() below.
    val hardcoreOn = mutableStateOf(false)

    // Locally tracked frame-limit toggle. 0 = Nominal (60fps cap),
    // 3 = Unlimited. Matches LimiterModeType / SpeedhackButton wiring.
    //
    // Default ON at app start (60fps cap). The state is held by this
    // singleton object so within an app session toggling once persists
    // across game launches — Main.start() re-applies it to native after
    // Settings.applyTo() runs on each VM init. Process restart resets to
    // the default (ON).
    val frameLimitOn = mutableStateOf(true)

    /** Renderer pill mode. Cycle: Auto → Hardware → Software → Auto.
     *
     *  - Auto = the default. Honours the wizard's backend choice
     *    (Main.renderer = "opengl" / "vulkan") and uses its HW path, but
     *    does NOT pin anything — emucore is free to swap to SW for
     *    things like SoftwareRendererFMVHack during FMVs, and the pill
     *    sync stays out of the way so the label keeps reading "Auto".
     *  - Hardware = pin HW on the picked backend.
     *  - Software = pin SW (display still on the picked backend's device).
     *
     *  We do NOT call NativeApp.renderAuto() here because it writes
     *  GSRendererType::Auto which then asks GSUtil::GetPreferredRenderer
     *  to pick a backend — on Android that resolves to OpenGL regardless
     *  of the wizard pick, silently rebuilding the device and (for Vulkan
     *  users) breaking the user's chosen backend. Auto's HW path is the
     *  same JNI as Hardware; the difference is purely label + sync. */
    enum class RendererMode { Auto, Hardware, Software }

    // Mirrored from native via NativeApp.isHardwareRenderer() on open +
    // on the achievements panel's 4s poll so an emucore-driven swap
    // (e.g. SoftwareRendererFMVHack flipping to SW during an FMV) doesn't
    // desync the Hardware / Software label. Sync is suppressed while in
    // Auto so the label stays "Auto" — the user picked "let it manage".
    val rendererMode = mutableStateOf(RendererMode.Auto)

    // OSD master toggle. Default ON because native-lib's initialize() turns
    // every OsdShow* bit on at first init; we only need to mirror the state
    // here so the pill label is right and re-toggling flips them all back.
    // Singleton state means the in-session preference persists across game
    // launches, matching the frame-limit pill pattern.
    private val osdShown = mutableStateOf(true)

    // Live RetroAchievements rich-presence string. Written by
    // AchievementsPanel's 4s poll (it already polls the achievements JSON
    // on the same cadence; one more JNI call is free). Read by
    // GameInfoHeader so the in-game pause panel reflects the current RP
    // line right under the game serial / star rating row.
    val richPresence = mutableStateOf("")

    // Tracks whether THIS overlay is the one that paused the VM. If the
    // user already had the VM paused (e.g. via toolbar) before opening
    // the overlay, closing the overlay shouldn't auto-resume.
    private var pausedByOverlay = false

    /** Build version string read at runtime from BuildVersion::GitRev
     *  via NativeApp.getBuildVersion(). Format
     *  "GitTagHi.GitTagMid.GitTagLo.ARMSX2Build-SNAPSHOT". Lazy so the
     *  JNI call defers to first use; cached for subsequent reads.
     *  Mirrored by SetupImpl so the wizard's title row stays in sync. */
    private val versionString: String by lazy {
        runCatching { NativeApp.getBuildVersion() }
            .getOrNull()
            ?.takeIf { it.isNotEmpty() }
            ?.let { "v$it" }
            ?: ""
    }

    /** Open the overlay. Pauses the VM. Safe to call when already open. */
    fun open() {
        if (WindowImpl.overlayVisible.value) return
        pausedByOverlay = (Main.eState.value == EmuState.RUNNING)
        if (pausedByOverlay) Main.pause()
        state.value = State.Root
        currentTab.value = Tab.PlayingNow
        // Re-hydrate settings from disk in case anything edited out-of-band.
        settingsState.value = ConfigStore.loadGlobal()
        // Sync pill state from native — covers emucore-driven swaps that
        // happened while the overlay was closed. Auto is sticky against
        // the sync (the user picked "let it decide", so we keep showing
        // Auto even though GS resolved it to HW underneath).
        if (rendererMode.value != RendererMode.Auto) {
            runCatching {
                rendererMode.value = if (NativeApp.isHardwareRenderer())
                    RendererMode.Hardware else RendererMode.Software
            }
        }
        WindowImpl.overlayVisible.value = true
    }

    private fun closeAndResume() {
        WindowImpl.overlayVisible.value = false
        state.value = State.Root
        // Always resume if the VM is paused — close-paths that should
        // preserve a paused VM (Change Disc picker, library, edit mode)
        // go through closeKeepingState instead. The earlier
        // pausedByOverlay gate stale-locked the VM after the user
        // bounced through closeKeepingState (e.g. entered edit mode then
        // re-opened pause): pausedByOverlay was cleared, so the next
        // Resume tap did nothing.
        if (Main.eState.value == EmuState.PAUSED) Main.resume()
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
        // as Resume Game. The dim layer fills the whole screen (cutout
        // included) so the partial-alpha aesthetic is uniform; only the
        // INNER content box gets displayCutoutPadding so headers / menu
        // rows aren't obscured by punch-hole or notch hardware.
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
            // Inner safe-area container — content laid out against this
            // box's edges automatically gets cutout-aware insets. Tap on
            // the dim band outside still falls through to the backdrop's
            // close-on-tap handler because this inner Box is non-clickable.
            Box(Modifier.fillMaxSize().displayCutoutPadding()) {
            // Top-left: game info, then (on Root) the tab strip and the
            // active tab's body stacked directly beneath it. Keeping the
            // strip and its content in the same column means tabs always
            // own their entries — no detached "strip up here, rows down
            // there" split. Submenu / confirm states fall through to the
            // bottom-left panel below.
            Column(
                modifier = Modifier
                    .align(Alignment.TopStart)
                    .fillMaxHeight()
                    .padding(20.dp)
                    .width(360.dp),
            ) {
                GameInfoHeader()
                if (state.value is State.Root) {
                    Spacer(Modifier.height(12.dp))
                    TabStrip()
                    Spacer(Modifier.height(6.dp))
                    // weight(1f) gives RootTabs the remaining vertical
                    // space, bounding Performance/Renderer's verticalScroll
                    // so it actually scrolls instead of expanding off-screen.
                    Box(modifier = Modifier.weight(1f)) {
                        RootTabs()
                    }
                }
            }

            // Top-right column: ARMSX2 branding + version, and below it the
            // achievements panel (visible on the Root state only — submenus
            // own the screen for confirms / pickers). Polls
            // NativeApp.getAchievementsJSON every few seconds while open so
            // freshly-unlocked rows appear without a manual reopen.
            Column(
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .fillMaxHeight()
                    .padding(20.dp)
                    .width(360.dp),
                horizontalAlignment = Alignment.End,
            ) {
                BrandHeader(modifier = Modifier)
                if (state.value is State.Root) {
                    Spacer(Modifier.height(12.dp))
                    Box(modifier = Modifier.weight(1f).fillMaxWidth()) {
                        AchievementsPanel(
                            modifier = Modifier.fillMaxWidth(),
                            onSignInClick = { state.value = State.AchievementsLogin },
                            onHardcoreToggle = {
                                if (hardcoreOn.value) {
                                    // Turn-off is safe (no VM reset) — apply
                                    // immediately. Native will fold the change
                                    // through Achievements::DisableHardcoreMode.
                                    NativeApp.setHardcoreMode(false)
                                    hardcoreOn.value = false
                                } else {
                                    state.value = State.HardcoreEnableConfirm
                                }
                            },
                        )
                    }
                }
            }

            // Bottom-left: confirm dialogs and slot pickers only. Root
            // content lives in the top-left column above with its tab
            // strip; this panel just hosts modal-ish flows that should
            // sit at the bottom of the screen (Exit / Reset confirms,
            // save-state slot grid).
            if (state.value !is State.Root) {
                // The login form needs more vertical room than the other
                // bottom-left states (it has 2 text fields + disclaimer +
                // buttons), and on landscape phones with the keyboard up
                // 75% of the screen isn't quite enough — content clips and
                // verticalScroll only barely engages. Give the login state
                // a taller box.
                val maxFrac = if (state.value is State.AchievementsLogin) 0.92f else 0.75f
                // Slot pickers (Save/Load) span the full screen width so
                // the 2-row horizontal grid actually reaches across. Other
                // states (confirms, login) stay at the 360dp left column.
                val isSlotPicker = state.value is State.SaveStateSlots
                    || state.value is State.LoadStateSlots
                Box(
                    Modifier
                        .align(Alignment.BottomStart)
                        .fillMaxHeight(maxFrac)
                        .let { if (isSlotPicker) it.fillMaxWidth() else it.width(360.dp) }
                        .padding(start = 20.dp, end = if (isSlotPicker) 20.dp else 0.dp, bottom = 20.dp, top = 20.dp),
                    contentAlignment = Alignment.BottomStart,
                ) {
                    when (state.value) {
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
                        is State.AchievementsLogin -> AchievementsLoginPanel(
                            onClose = { state.value = State.Root },
                        )
                        is State.HardcoreSaveStateBlocked -> HardcoreBlockedBubble()
                        is State.HardcoreEnableConfirm -> Unit // rendered fullscreen below
                        is State.Root -> Unit
                    }
                }
            }

            // PS2-BIOS-style fullscreen confirm for enabling hardcore mode.
            // Painted on top of everything else, so it eats taps and the
            // user must explicitly confirm or cancel before returning to
            // the menu. Enabling hardcore resets the running game (per
            // upstream Achievements::ResetHardcoreMode) so we want the
            // user to know exactly what's about to happen.
            if (state.value is State.HardcoreEnableConfirm) {
                HardcoreEnableConfirmFullscreen()
            }
            } // displayCutoutPadding inner box
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
                    // PS2 boxart matches the 0.7 aspect cell — Crop fills.
                    // PS1 jewel-case covers are squarer; Fit + Center
                    // letterboxes them inside the same cell so the full
                    // art is visible without cropping.
                    val scale = when (cached?.platform) {
                        com.armsx2.GamePlatform.PS1 -> ContentScale.Fit
                        else -> ContentScale.Crop
                    }
                    SubcomposeAsyncImage(
                        model = ImageRequest.Builder(context).data(coverUrl).crossfade(true).build(),
                        contentDescription = "$title cover",
                        contentScale = scale,
                        alignment = Alignment.Center,
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
                    if (hardcoreOn.value) {
                        Spacer(Modifier.width(6.dp))
                        HardcoreBadge()
                    }
                }
                // Live RetroAchievements rich-presence subtitle. Mirrors
                // what RA's website shows under your active game; we surface
                // it locally so the user can see what the server is being
                // told. AchievementsPanel's 4s poll owns the writes.
                val rp = richPresence.value
                if (rp.isNotEmpty()) {
                    Spacer(Modifier.height(3.dp))
                    MarqueeRichPresence(rp)
                }
            }
        }
    }

    /** One-line rich-presence subtitle. Renders directly when the string
     *  fits within the available width; runs a back-and-forth ping-pong
     *  marquee when it overflows. Holds 1s at each end so the user has
     *  time to read both the start and the tail before the slide flips
     *  direction. Linear scroll between holds so the motion reads as a
     *  deliberate reveal rather than easing. */
    @Composable
    private fun MarqueeRichPresence(text: String) {
        // Measure the text's INTRINSIC width via TextMeasurer. onSizeChanged
        // on the rendered Text reports the constrained width (== container)
        // when the parent caps it, which makes overflow detection a no-op
        // and the marquee never starts. TextMeasurer gives us the actual
        // size the text wants to be, regardless of layout pressure.
        val style = remember {
            TextStyle(color = Color(0xFFBBBBBB), fontSize = 11.sp)
        }
        val measurer = rememberTextMeasurer()
        val intrinsicTextWidth = remember(text, style) {
            measurer.measure(text = text, style = style, softWrap = false, maxLines = 1).size.width
        }
        var containerWidth by remember(text) { mutableIntStateOf(0) }
        val overflowPx = (intrinsicTextWidth - containerWidth).coerceAtLeast(0)
        val needsMarquee = overflowPx > 0 && containerWidth > 0

        // Ping-pong timing: 1s hold → scroll → 1s hold → scroll back.
        // Scroll speed pinned at ~60 px/sec so a long string takes a
        // perceptible amount of time without dragging.
        val holdMs = 1000
        val scrollMs = if (needsMarquee) (overflowPx * 1000 / 60).coerceAtLeast(800) else 0
        val totalMs = (holdMs * 2 + scrollMs * 2).coerceAtLeast(1)
        val maxOffset = overflowPx.toFloat()

        val transition = rememberInfiniteTransition(label = "rp-marquee")
        val offsetPx by transition.animateFloat(
            initialValue = 0f,
            targetValue = 0f,
            animationSpec = infiniteRepeatable(
                animation = keyframes {
                    durationMillis = totalMs
                    0f at 0
                    0f at holdMs                                using LinearEasing
                    -maxOffset at holdMs + scrollMs              using LinearEasing
                    -maxOffset at holdMs + scrollMs + holdMs    using LinearEasing
                    0f at totalMs                                using LinearEasing
                },
                repeatMode = RepeatMode.Restart,
            ),
            label = "rp-offset",
        )

        Box(
            modifier = Modifier
                .fillMaxWidth()
                .clipToBounds()
                .onSizeChanged { containerWidth = it.width },
        ) {
            Text(
                text = text,
                color = Color(0xFFBBBBBB),
                fontSize = 11.sp,
                maxLines = 1,
                softWrap = false,
                overflow = TextOverflow.Visible,
                modifier = Modifier
                    .offset {
                        IntOffset(if (needsMarquee) offsetPx.toInt() else 0, 0)
                    },
            )
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

    /** Small red badge displayed next to the extension badge when
     *  RetroAchievements Hardcore mode is active. Same shape as
     *  ExtensionBadge so they line up visually. */
    @Composable
    private fun HardcoreBadge() {
        Box(
            Modifier
                .clip(RoundedCornerShape(4.dp))
                .background(Color(0xFFB22222))
                .padding(horizontal = 5.dp, vertical = 1.dp),
        ) {
            Text(
                "HC",
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
                Text(versionString, color = Color(0xFF888888), fontSize = 11.sp)
            }
            Spacer(Modifier.width(8.dp))
            Image(
                painter = painterResource(id = R.drawable.savetowerforeground),
                contentDescription = null,
                modifier = Modifier.size(32.dp),
            )
        }
    }

    /** Active tab body. Rendered in the top-left column directly under
     *  TabStrip, so the strip and its entries stay visually attached.
     *  Width and horizontal padding come from the parent column. */
    @Composable
    private fun RootTabs() {
        when (currentTab.value) {
            Tab.PlayingNow -> PlayingNowTab()
            Tab.Performance -> PerformanceTab(settingsState)
            Tab.Renderer -> RendererTab(settingsState)
            Tab.Recompiler -> RecompilerTab(settingsState)
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

    /** Playing Now — pause-menu actions laid out as a 4-wide bubble grid.
     *  Three rows of four cells; the last row trails with two empty slots
     *  so cell widths stay constant across all rows. Fits without
     *  scrolling on phone landscape. Primary (Resume) and Danger (Close)
     *  accents land the eye on the most common entry and the destructive
     *  exit. Toggleable bubbles (Renderer, Frame Limit) carry their
     *  current value on a second line. */
    @Composable
    private fun PlayingNowTab() {
        Column(
            modifier = Modifier.fillMaxWidth(),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            // Row 1: primary + save/load + change disc.
            BubbleRow {
                BubbleButton(
                    "Resume",
                    LineAwesomeIcons.PlaySolid,
                    accent = BubbleAccent.Primary,
                    modifier = Modifier.weight(1f),
                ) { closeAndResume() }
                BubbleButton(
                    "Save State",
                    LineAwesomeIcons.SaveSolid,
                    dim = hardcoreOn.value,
                    modifier = Modifier.weight(1f),
                ) {
                    state.value = if (hardcoreOn.value)
                        State.HardcoreSaveStateBlocked else State.SaveStateSlots
                }
                BubbleButton(
                    "Load State",
                    LineAwesomeIcons.FolderOpenSolid,
                    dim = hardcoreOn.value,
                    modifier = Modifier.weight(1f),
                ) {
                    state.value = if (hardcoreOn.value)
                        State.HardcoreSaveStateBlocked else State.LoadStateSlots
                }
                BubbleButton(
                    "Change Disc",
                    LineAwesomeIcons.CompactDiscSolid,
                    modifier = Modifier.weight(1f),
                ) {
                    val intent = Intent(Intent.ACTION_GET_CONTENT)
                    intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, false)
                    intent.setType("*/*")
                    Main.instance?.openFileAction?.launch(intent)
                    closeKeepingState()
                }
            }
            // Row 2: library + renderer + frame limit + touch layout.
            BubbleRow {
                BubbleButton(
                    "Library",
                    LineAwesomeIcons.ThLargeSolid,
                    modifier = Modifier.weight(1f),
                ) {
                    if (Main.eState.value == EmuState.PAUSED) Main.resume()
                    WindowImpl.showLibrary.value = true
                    closeKeepingState()
                }
                BubbleButton(
                    "Renderer",
                    LineAwesomeIcons.CubeSolid,
                    stateLine = when (rendererMode.value) {
                        RendererMode.Auto -> "Auto"
                        RendererMode.Hardware -> "Hardware"
                        RendererMode.Software -> "Software"
                    },
                    accent = BubbleAccent.Active,
                    modifier = Modifier.weight(1f),
                ) {
                    val backendHw: () -> Unit = {
                        if (Main.renderer.value == "vulkan")
                            Main.renderVulkan() else Main.renderOpenGL()
                    }
                    rendererMode.value = when (rendererMode.value) {
                        // Auto → Hardware: pin HW on the picked backend.
                        // No JNI needed if we're already in HW (poll-sync
                        // means the native side is already there); call
                        // the HW JNI anyway to handle the case where the
                        // engine had auto-swapped to SW (e.g. FMV) but the
                        // user wants to lock back to HW.
                        RendererMode.Auto -> {
                            backendHw()
                            RendererMode.Hardware
                        }
                        RendererMode.Hardware -> {
                            Main.renderSoftware()
                            RendererMode.Software
                        }
                        // Software → Auto: bring the renderer back to the
                        // backend's HW default. Same JNI as Hardware; the
                        // pill label differs and the sync leaves us alone.
                        RendererMode.Software -> {
                            backendHw()
                            RendererMode.Auto
                        }
                    }
                }
                BubbleButton(
                    "Frame Limit",
                    LineAwesomeIcons.TachometerAltSolid,
                    stateLine = if (frameLimitOn.value) "On" else "Off",
                    accent = if (frameLimitOn.value)
                        BubbleAccent.Active else BubbleAccent.Normal,
                    modifier = Modifier.weight(1f),
                ) {
                    frameLimitOn.value = !frameLimitOn.value
                    // Update both the BASE layer (so a future VM init /
                    // settings reload sees the new value) and call
                    // speedhackLimitermode for immediate effect on the
                    // running VM.
                    NativeApp.setSetting(
                        "EmuCore/GS", "FrameLimitEnable", "bool",
                        frameLimitOn.value.toString()
                    )
                    NativeApp.speedhackLimitermode(if (frameLimitOn.value) 0 else 3)
                }
                BubbleButton(
                    "Touch Layout",
                    LineAwesomeIcons.ThLargeSolid,
                    modifier = Modifier.weight(1f),
                ) {
                    com.armsx2.ui.touch.TouchControls.ensureLoaded()
                    com.armsx2.ui.touch.TouchControls.editMode.value = true
                    // Close the pause overlay so the editor owns the
                    // screen. closeKeepingState leaves the VM paused — the
                    // user can resume from the Save / Discard chips in
                    // the editor.
                    closeKeepingState()
                }
            }
            // Row 3: OSD master toggle, reset, close, trailing spacer.
            BubbleRow {
                BubbleButton(
                    "OSD",
                    if (osdShown.value) LineAwesomeIcons.EyeSolid
                    else LineAwesomeIcons.EyeSlashSolid,
                    stateLine = if (osdShown.value) "On" else "Off",
                    accent = if (osdShown.value)
                        BubbleAccent.Active else BubbleAccent.Normal,
                    modifier = Modifier.weight(1f),
                ) {
                    osdShown.value = !osdShown.value
                    NativeApp.osdShowAll(osdShown.value)
                }
                BubbleButton(
                    "Reset",
                    LineAwesomeIcons.RedoAltSolid,
                    modifier = Modifier.weight(1f),
                ) { state.value = State.ResetConfirm }
                BubbleButton(
                    "Close Game",
                    LineAwesomeIcons.PowerOffSolid,
                    accent = BubbleAccent.Danger,
                    modifier = Modifier.weight(1f),
                ) { state.value = State.ExitConfirm }
                Spacer(Modifier.weight(1f))
            }
        }
    }

    /** Even-spaced four-cell row, used by [PlayingNowTab] for grid rows. */
    @Composable
    private fun BubbleRow(content: @Composable RowScope.() -> Unit) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            content = content,
        )
    }

    /** Visual variants for [BubbleButton]. Normal is the default surface;
     *  Primary tints the bubble with the PS2-blue accent (Resume); Active
     *  is a softer accent used for toggleable bubbles whose state is
     *  currently "on"; Danger paints the bubble red for destructive
     *  actions. */
    private enum class BubbleAccent { Normal, Primary, Active, Danger }

    /** Square bubble action. Icon centered on top, label below, optional
     *  state line under the label (e.g. "On" / "Software") so toggleable
     *  bubbles communicate their current value without separate text. */
    @Composable
    private fun BubbleButton(
        label: String,
        icon: ImageVector,
        modifier: Modifier = Modifier,
        stateLine: String? = null,
        accent: BubbleAccent = BubbleAccent.Normal,
        dim: Boolean = false,
        onClick: () -> Unit,
    ) {
        // Per-variant palette. Dim wins over any accent so a Hardcore-mode
        // Save State row still reads "blocked".
        val bg: Color
        val border: Color
        val fg: Color
        when {
            dim -> {
                bg = Color(0xFF1F1F1F)
                border = Color(0xFF2E2E2E)
                fg = Color(0xFF666666)
            }
            accent == BubbleAccent.Primary -> {
                bg = Colors.pasx2_blue.copy(alpha = 0.22f)
                border = Colors.pasx2_blue.copy(alpha = 0.65f)
                fg = Color.White
            }
            accent == BubbleAccent.Active -> {
                bg = Color(0xFF222F40)
                border = Colors.pasx2_blue.copy(alpha = 0.50f)
                fg = Color.White
            }
            accent == BubbleAccent.Danger -> {
                bg = Color(0xFF2E1818)
                border = Color(0xFFFF6B6B).copy(alpha = 0.55f)
                fg = Color(0xFFFF8B8B)
            }
            else -> {
                bg = Color(0xFF1F2123)
                border = Color.White.copy(alpha = 0.10f)
                fg = Color.White
            }
        }

        Column(
            modifier = modifier
                .aspectRatio(1.35f)
                .clip(RoundedCornerShape(10.dp))
                .background(bg)
                .border(1.dp, border, RoundedCornerShape(10.dp))
                .clickable(enabled = !dim, onClick = onClick)
                .padding(horizontal = 4.dp, vertical = 4.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            Image(
                imageVector = icon,
                contentDescription = null,
                colorFilter = ColorFilter.tint(fg),
                modifier = Modifier.size(16.dp),
            )
            Spacer(Modifier.height(2.dp))
            Text(
                label,
                color = fg,
                fontSize = 9.sp,
                fontWeight = FontWeight.Bold,
                textAlign = TextAlign.Center,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            if (stateLine != null) {
                // Active accent → state line in PS2 blue (matches the
                // ToggleBubble "On" treatment in the Performance grid).
                // Other variants keep the muted-fg colour so Normal bubbles
                // don't accidentally read as active.
                val stateColor = if (accent == BubbleAccent.Active)
                    Colors.pasx2_blue else fg.copy(alpha = 0.7f)
                Text(
                    stateLine,
                    color = stateColor,
                    fontSize = 10.sp,
                    fontWeight = FontWeight.Bold,
                    textAlign = TextAlign.Center,
                    maxLines = 1,
                )
            }
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
                // Dedicated autosave slot — keeps numbered slots 0-9
                // user-controlled. SaveStatePicker (Load mode) surfaces
                // this state via the Autosave tile when present.
                NativeApp.saveAutosaveState()
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

    /** Modern red-bubble hint shown when the user taps Save / Load State
     *  while hardcore mode is active. Fits inside the bottom-left modal
     *  box; auto-dismisses on tap. */
    @Composable
    private fun HardcoreBlockedBubble() {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(10.dp))
                .background(Color(0xFF5A1A1A))
                .clickable { state.value = State.Root }
                .padding(14.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text("⛔", fontSize = 16.sp, color = Color(0xFFFF6B6B))
                Spacer(Modifier.width(8.dp))
                Text(
                    "Disabled in Hardcore",
                    color = Color.White,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Bold,
                )
            }
            Spacer(Modifier.height(6.dp))
            Text(
                "Save states and load states are blocked while RetroAchievements Hardcore mode is active. Disable Hardcore from the Achievements panel to use them.",
                color = Color(0xFFEEDDDD),
                fontSize = 11.sp,
            )
            Spacer(Modifier.height(8.dp))
            Text(
                "Tap to dismiss",
                color = Color(0xFFFF6B6B),
                fontSize = 10.sp,
            )
        }
    }

    /** Fullscreen confirmation modal for enabling hardcore mode. Styled
     *  to evoke the PS2 BIOS UI — centered black panel with a thin double
     *  border and chunky monospace-feeling title. The action enables the
     *  Achievements/HardcoreMode flag in the BASE settings layer; native
     *  ApplySettings folds that into Achievements::ResetHardcoreMode
     *  which resets the running game. */
    @Composable
    private fun HardcoreEnableConfirmFullscreen() {
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(Color(0xCC000000))
                // Eat taps so background controls don't trigger.
                .clickable(
                    indication = null,
                    interactionSource = remember { androidx.compose.foundation.interaction.MutableInteractionSource() },
                ) {},
            contentAlignment = Alignment.Center,
        ) {
            Column(
                modifier = Modifier
                    .width(420.dp)
                    .clip(RoundedCornerShape(2.dp))
                    .background(Color(0xFF0A0A18))
                    .border(2.dp, Color(0xFF8888AA), RoundedCornerShape(2.dp))
                    .padding(2.dp)
                    .border(1.dp, Color(0xFF333366), RoundedCornerShape(2.dp))
                    .padding(20.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(
                    "ENABLE HARDCORE MODE",
                    color = Color(0xFFFF8888),
                    fontSize = 18.sp,
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 2.sp,
                )
                Spacer(Modifier.height(2.dp))
                Box(
                    Modifier
                        .fillMaxWidth()
                        .height(1.dp)
                        .background(Color(0xFF8888AA)),
                )
                Spacer(Modifier.height(14.dp))
                Text(
                    "Hardcore mode disables save states, load states, and cheats. Achievements unlocked while hardcore is active are recorded as such on RetroAchievements.",
                    color = Color(0xFFDDDDEE),
                    fontSize = 13.sp,
                )
                Spacer(Modifier.height(8.dp))
                Text(
                    "The current game will reset.",
                    color = Color(0xFFFFAAAA),
                    fontSize = 13.sp,
                    fontWeight = FontWeight.Bold,
                )
                Spacer(Modifier.height(20.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    BiosLikeButton(
                        label = "CANCEL",
                        primary = false,
                        modifier = Modifier.weight(1f),
                    ) { state.value = State.Root }
                    BiosLikeButton(
                        label = "ENABLE",
                        primary = true,
                        modifier = Modifier.weight(1f),
                    ) {
                        NativeApp.setHardcoreMode(true)
                        hardcoreOn.value = true
                        state.value = State.Root
                    }
                }
            }
        }
    }

    @Composable
    private fun BiosLikeButton(
        label: String,
        primary: Boolean,
        modifier: Modifier = Modifier,
        onClick: () -> Unit,
    ) {
        val bg = if (primary) Color(0xFF5A1A1A) else Color(0xFF1A1A2A)
        val border = if (primary) Color(0xFFFF6B6B) else Color(0xFF8888AA)
        val fg = if (primary) Color(0xFFFFCCCC) else Color(0xFFDDDDEE)
        Box(
            modifier = modifier
                .clip(RoundedCornerShape(2.dp))
                .background(bg)
                .border(1.dp, border, RoundedCornerShape(2.dp))
                .clickable(onClick = onClick)
                .padding(vertical = 12.dp),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                label,
                color = fg,
                fontSize = 14.sp,
                fontWeight = FontWeight.Bold,
                letterSpacing = 2.sp,
            )
        }
    }

    /** Single menu row. Compact (24dp) so all 10 root items fit without
     *  scrolling on phone landscape. Background is a left-anchored
     *  alpha gradient that "auras" the text and matches the divider
     *  fade direction. Danger variant tints text red for destructive
     *  actions (Close Game / Exit Without Saving). Optional icon
     *  rendered on the left, tinted with the same color as the label
     *  text — mirrors PCSX2 ImGui FullscreenUI's leading-icon menu
     *  rows in DrawPauseMenu. */
    @Composable
    private fun MenuRow(
        label: String,
        icon: ImageVector? = null,
        danger: Boolean = false,
        dim: Boolean = false,
        onClick: () -> Unit,
    ) {
        val textColor = when {
            dim -> Color(0xFF666666)
            danger -> Color(0xFFFF6B6B)
            else -> Color.White
        }
        val auraStart = when {
            dim -> Color.White.copy(alpha = 0.02f)
            danger -> Color(0xFFFF6B6B).copy(alpha = 0.10f)
            else -> Color.White.copy(alpha = 0.06f)
        }
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
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (icon != null) {
                    Image(
                        imageVector = icon,
                        contentDescription = null,
                        colorFilter = ColorFilter.tint(textColor),
                        modifier = Modifier.size(14.dp),
                    )
                    Spacer(Modifier.width(8.dp))
                } else {
                    // Reserve same space so labels align across rows w/
                    // and w/o icons (Confirm-screen Back rows skip icons).
                    Spacer(Modifier.width(22.dp))
                }
                Text(label, color = textColor, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            }
        }
    }
}
