package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.armsx2.config.Settings
import com.armsx2.ui.InGameOverlay

/**
 * Performance section of the in-game settings overlay.
 *
 * Mutates the live [Settings] state and routes the write through
 * [InGameOverlay.saveSettings], which picks the global or per-game
 * storage tier based on the overlay's current scope. Applies via
 * [Settings.applyTo] so toggles take effect immediately on a running
 * VM. Every visible setting maps 1-1 onto an EmuCore key (see Settings
 * field comments for the exact `<section>/<key>`).
 *
 * Column + verticalScroll instead of LazyColumn so the tab can sit
 * inside the wrap-content RootTabs container without needing a hard
 * height bound. List is short (~9 rows) so non-lazy is fine.
 */
@Composable
fun PerformanceTab(state: MutableState<Settings>) {
    val s = state.value
    val scroll = remember { ScrollState(0) }

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scroll),
    ) {
        IntSliderRow(
            label = "EE Cycle Rate",
            value = s.eeCycleRate,
            min = -3,
            max = 3,
            description = "CPU overclock/underclock. Higher can help CPU-bound games but may break timing.",
            valueFormatter = { rate ->
                when (rate) {
                    -3 -> "50%"
                    -2 -> "60%"
                    -1 -> "75%"
                    0 -> "100%"
                    1 -> "130%"
                    2 -> "180%"
                    3 -> "300%"
                    else -> "$rate"
                }
            },
            onChange = { apply(s.copy(eeCycleRate = it)) },
        )
        SettingsDivider()
        IntSliderRow(
            label = "EE Cycle Skip",
            value = s.eeCycleSkip,
            min = 0,
            max = 3,
            description = "Skips EE cycles for speed. Can cause stutter, physics bugs, or crashes.",
            onChange = { apply(s.copy(eeCycleSkip = it)) },
        )
        SettingsDivider()
        // Speed Limit / Custom FPS — caps emulation speed as a % of native
        // (100% ≈ 60fps NTSC / 50fps PAL). Only effective with the Frame
        // Limiter on. Driven by a preset index; stores the actual percent.
        run {
            val speedPresets = listOf(25, 50, 75, 100, 150, 200, 300)
            val idx = speedPresets.indexOf(s.nominalSpeedPercent).let { if (it < 0) 3 else it }
            IntSliderRow(
                label = "FPS / Speed Limit",
                value = idx,
                min = 0,
                max = speedPresets.size - 1,
                description = "Caps emulation speed as a percent of the game's native rate.",
                valueFormatter = { "${speedPresets[it]}%" },
                onChange = { apply(s.copy(nominalSpeedPercent = speedPresets[it])) },
            )
        }
        SettingsDivider()
        HelpText("Compatibility shortcuts. Leave Game Fixes off unless a game needs one of the fixes below.")
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            BubbleGridRow {
                ToggleBubble("Skip BIOS", s.enableFastBoot, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableFastBoot = it))
                }
                ToggleBubble("Game Fixes", s.enableGameFixes, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = it))
                }
                ToggleBubble("Skip MPEG", s.gamefixSkipMpeg, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixSkipMpeg = it))
                }
                ToggleBubble("FMV Software", s.gamefixSoftwareRendererFmv, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixSoftwareRendererFmv = it))
                }
            }
            BubbleGridRow {
                ToggleBubble("EE Timing", s.gamefixEETiming, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixEETiming = it))
                }
                ToggleBubble("Instant DMA", s.gamefixInstantDma, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixInstantDma = it))
                }
                ToggleBubble("Blit FPS", s.gamefixBlitInternalFps, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixBlitInternalFps = it))
                }
                Spacer(Modifier.weight(1f))
            }
        }
        HelpText(
            "Skip BIOS - bypasses the PS2 startup screen.\n" +
                "Game Fixes - master switch for manual compatibility fixes.\n" +
                "Skip MPEG - skips problematic video playback.\n" +
                "FMV Software - switches FMVs to software rendering.\n" +
                "EE Timing - adjusts CPU timing for sensitive games.\n" +
                "Instant DMA - completes DMA transfers immediately.\n" +
                "Blit FPS - uses PCSX2's internal FPS blit workaround."
        )
        SettingsDivider()
        Spacer(Modifier.height(8.dp))
        // On/Off toggles as a 4-wide bubble grid, matching the Playing-Now
        // action grid in InGameOverlay. Two rows of four cells — last cell
        // is a Spacer so the seven toggles keep uniform widths across both
        // rows. Labels are abbreviated to fit the ~74dp cell.
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            BubbleGridRow {
                ToggleBubble("MTVU", s.mtvu, modifier = Modifier.weight(1f)) {
                    apply(s.copy(mtvu = it))
                }
                ToggleBubble("Instant VU1", s.vu1Instant, modifier = Modifier.weight(1f)) {
                    apply(s.copy(vu1Instant = it))
                }
                ToggleBubble("VU Flag Hack", s.vuFlagHack, modifier = Modifier.weight(1f)) {
                    apply(s.copy(vuFlagHack = it))
                }
                ToggleBubble("Fast CDVD", s.fastCDVD, modifier = Modifier.weight(1f)) {
                    apply(s.copy(fastCDVD = it))
                }
            }
            BubbleGridRow {
                ToggleBubble("INTC Stat", s.intcStat, modifier = Modifier.weight(1f)) {
                    apply(s.copy(intcStat = it))
                }
                ToggleBubble("Wait Loop", s.waitLoop, modifier = Modifier.weight(1f)) {
                    apply(s.copy(waitLoop = it))
                }
                ToggleBubble("Frame Limiter", s.frameLimitEnable, modifier = Modifier.weight(1f)) {
                    apply(s.copy(frameLimitEnable = it))
                }
                // ARMSX2 perf-knob: A/B disable for the arm64 VU1 JIT
                // NEON peephole fusions. Default on; flip off to confirm
                // whether a per-game regression traces back to our
                // matrix-transform / cross-product fusion peepholes.
                // Block recompilation is not invalidated on toggle —
                // already-compiled blocks keep their fused path; new
                // blocks pick up the new gate. Restart the game for a
                // clean A/B (or hit the recompiler tab to flip a CPU off
                // then on, which forces a cache rebuild).
                ToggleBubble("VU NEON Fusions", s.vuNeonFusions, modifier = Modifier.weight(1f)) {
                    apply(s.copy(vuNeonFusions = it))
                }
            }
            // Heavy / aggressive perf levers — off by default, may break
            // some games. Effective on next block recompile; for a clean
            // A/B, restart the game (or bounce a CPU recompiler toggle
            // off→on to force a JIT cache rebuild).
            //
            //   Skip VU Stall Sim — drops the vu1_TestPipes_VU1 BL
            //     entirely. Was 19-32% of total CPU on Futurama / GoW2 /
            //     Ape Escape 3. Breaks games that depend on accurate
            //     FMAC / FDIV / EFU / IALU pipeline-stall timing
            //     (glitched models, missing geometry, audio crackle).
            //   Defer VU Writes — VF stores held in the NEON cache,
            //     committed at flush sites. Saves 1 Str per FMAC pair on
            //     transforms. Known to break SH2 graphics (cross-pair
            //     coherence) and similar.
            BubbleGridRow {
                ToggleBubble("Skip VU Stall Sim", s.vuSkipStallSim, modifier = Modifier.weight(1f)) {
                    apply(s.copy(vuSkipStallSim = it))
                }
                ToggleBubble("Defer VU Writes", s.vuDeferredWrites, modifier = Modifier.weight(1f)) {
                    apply(s.copy(vuDeferredWrites = it))
                }
                Spacer(Modifier.weight(1f))
                Spacer(Modifier.weight(1f))
            }
        }
    }
}
