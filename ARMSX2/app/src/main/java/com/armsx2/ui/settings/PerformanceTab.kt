package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.ui.res.stringResource
import com.armsx2.R
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
    ControllerAutoScroll(scroll)

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scroll),
    ) {
        // Speedhack profile presets. Equality against s.copy(...) means the
        // segment auto-reflects "Custom" once the user tweaks any speedhack below.
        run {
            val safe = s.copy(eeCycleRate = 0, eeCycleSkip = 0, mtvu = true, vu1Instant = true,
                vuFlagHack = true, intcStat = true, waitLoop = true, fastCDVD = false)
            val fast = s.copy(eeCycleRate = 0, eeCycleSkip = 2, mtvu = true, vu1Instant = true,
                vuFlagHack = true, intcStat = true, waitLoop = true, fastCDVD = true)
            // -1 = neither preset matches (custom): no segment highlighted.
            val idx = when (s) { safe -> 0; fast -> 1; else -> -1 }
            SegmentedRow(
                label = stringResource(R.string.perf_speedhack_profile),
                options = listOf(stringResource(R.string.perf_speedhack_optimal), stringResource(R.string.perf_speedhack_fast)),
                selectedIndex = idx,
                onChange = { when (it) { 0 -> apply(safe); 1 -> apply(fast) } },
            )
        }
        HelpText(stringResource(R.string.settings_perf_preset_help))
        SettingsDivider()
        IntSliderRow(
            label = stringResource(R.string.perf_ee_cycle_rate),
            value = s.eeCycleRate,
            min = -3,
            max = 3,
            description = stringResource(R.string.perf_ee_cycle_rate_help),
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
            label = stringResource(R.string.perf_ee_cycle_skip),
            value = s.eeCycleSkip,
            min = 0,
            max = 3,
            description = stringResource(R.string.perf_ee_cycle_skip_help),
            onChange = { apply(s.copy(eeCycleSkip = it)) },
        )
        SettingsDivider()
        // Recompiler float-clamping accuracy (PCSX2 parity). Higher = more
        // accurate float handling (fixes SPS / missing geometry / VU glitches)
        // at a speed cost. Needs a recompiler reset, so restart the game.
        SegmentedRow(
            label = stringResource(R.string.perf_ee_fpu_clamping),
            options = listOf(stringResource(R.string.perf_clamp_none), stringResource(R.string.perf_clamp_normal), stringResource(R.string.perf_clamp_extra), stringResource(R.string.perf_clamp_full)),
            selectedIndex = s.eeClampMode.coerceIn(0, 3),
            description = stringResource(R.string.perf_ee_fpu_clamping_help),
            onChange = { apply(s.copy(eeClampMode = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = stringResource(R.string.perf_vu_clamping),
            options = listOf(stringResource(R.string.perf_clamp_none), stringResource(R.string.perf_clamp_normal), stringResource(R.string.perf_clamp_extra), stringResource(R.string.perf_clamp_extra_sign)),
            selectedIndex = s.vuClampMode.coerceIn(0, 3),
            description = stringResource(R.string.perf_vu_clamping_help),
            onChange = { apply(s.copy(vuClampMode = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = stringResource(R.string.perf_ee_fpu_round_mode),
            options = listOf(stringResource(R.string.perf_round_nearest), stringResource(R.string.perf_round_negative), stringResource(R.string.perf_round_positive), stringResource(R.string.perf_round_chop)),
            selectedIndex = s.eeFpuRoundMode.coerceIn(0, 3),
            description = stringResource(R.string.perf_ee_fpu_round_mode_help),
            onChange = { apply(s.copy(eeFpuRoundMode = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = stringResource(R.string.perf_vu0_round_mode),
            options = listOf("Nearest", "Negative", "Positive", "Chop"),
            selectedIndex = s.vu0RoundMode.coerceIn(0, 3),
            description = stringResource(R.string.perf_vu0_round_mode_help),
            onChange = { apply(s.copy(vu0RoundMode = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = stringResource(R.string.perf_vu1_round_mode),
            options = listOf("Nearest", "Negative", "Positive", "Chop"),
            selectedIndex = s.vu1RoundMode.coerceIn(0, 3),
            description = stringResource(R.string.perf_vu1_round_mode_help),
            onChange = { apply(s.copy(vu1RoundMode = it)) },
        )
        SettingsDivider()
        // Speed Limit / Custom FPS — caps emulation speed as a % of native
        // (100% ≈ 60fps NTSC / 50fps PAL). Only effective with the Frame
        // Limiter on. Driven by a preset index; stores the actual percent.
        run {
            val speedPresets = listOf(25, 50, 75, 100, 150, 200, 300)
            val idx = speedPresets.indexOf(s.nominalSpeedPercent).let { if (it < 0) 3 else it }
            IntSliderRow(
                label = stringResource(R.string.perf_fps_speed_limit),
                value = idx,
                min = 0,
                max = speedPresets.size - 1,
                description = stringResource(R.string.perf_fps_speed_limit_help),
                valueFormatter = { "${speedPresets[it]}%" },
                onChange = { apply(s.copy(nominalSpeedPercent = speedPresets[it])) },
            )
        }
        SettingsDivider()
        IntSliderRow(
            label = stringResource(R.string.perf_frame_skip),
            value = s.frameSkip,
            min = 0,
            max = 5,
            description = stringResource(R.string.perf_frame_skip_help),
            valueFormatter = { if (it == 0) "Off" else "Skip $it" },
            onChange = { apply(s.copy(frameSkip = it)) },
        )
        SettingsDivider()
        HelpText(stringResource(R.string.settings_perf_gamefix_help))
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            BubbleGridRow {
                ToggleBubble(stringResource(R.string.perf_gamefix_skip_bios), s.enableFastBoot, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableFastBoot = it))
                }
                ToggleBubble(stringResource(R.string.perf_gamefix_game_fixes), s.enableGameFixes, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = it))
                }
                ToggleBubble(stringResource(R.string.perf_gamefix_skip_mpeg), s.gamefixSkipMpeg, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixSkipMpeg = it))
                }
                ToggleBubble(stringResource(R.string.perf_gamefix_fmv_software), s.gamefixSoftwareRendererFmv, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixSoftwareRendererFmv = it))
                }
            }
            BubbleGridRow {
                ToggleBubble(stringResource(R.string.perf_gamefix_ee_timing), s.gamefixEETiming, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixEETiming = it))
                }
                ToggleBubble(stringResource(R.string.perf_gamefix_instant_dma), s.gamefixInstantDma, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixInstantDma = it))
                }
                ToggleBubble(stringResource(R.string.perf_gamefix_blit_fps), s.gamefixBlitInternalFps, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixBlitInternalFps = it))
                }
                ToggleBubble(stringResource(R.string.perf_gamefix_fpu_multiply), s.gamefixFpuMul, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixFpuMul = it))
                }
            }
            BubbleGridRow {
                ToggleBubble(stringResource(R.string.perf_gamefix_oph_flag), s.gamefixOphFlag, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixOphFlag = it))
                }
                ToggleBubble(stringResource(R.string.perf_gamefix_gif_fifo), s.gamefixGifFifo, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixGifFifo = it))
                }
                ToggleBubble(stringResource(R.string.perf_gamefix_dma_busy), s.gamefixDmaBusy, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixDmaBusy = it))
                }
                ToggleBubble(stringResource(R.string.perf_gamefix_vif1_stall), s.gamefixVif1Stall, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixVif1Stall = it))
                }
            }
            BubbleGridRow {
                ToggleBubble(stringResource(R.string.perf_gamefix_ibit), s.gamefixIbit, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixIbit = it))
                }
                ToggleBubble(stringResource(R.string.perf_gamefix_full_vu0_sync), s.gamefixFullVu0Sync, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixFullVu0Sync = it))
                }
                ToggleBubble(stringResource(R.string.perf_gamefix_vu_add_sub), s.gamefixVuAddSub, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixVuAddSub = it))
                }
                ToggleBubble(stringResource(R.string.perf_gamefix_vu_overflow), s.gamefixVuOverflow, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixVuOverflow = it))
                }
            }
            BubbleGridRow {
                ToggleBubble(stringResource(R.string.perf_gamefix_extra_xgkick), s.gamefixXgkick, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixXgkick = it))
                }
                ToggleBubble(stringResource(R.string.perf_gamefix_goemon_tlb), s.gamefixGoemonTlb, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixGoemonTlb = it))
                }
                ToggleBubble(stringResource(R.string.perf_gamefix_vu_sync), s.gamefixVuSync, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableGameFixes = true, gamefixVuSync = it))
                }
                Spacer(Modifier.weight(1f))
            }
        }
        HelpText(stringResource(R.string.perf_gamefix_help))
        SettingsDivider()
        Spacer(Modifier.height(8.dp))
        // On/Off toggles as a 4-wide bubble grid, matching the Playing-Now
        // action grid in InGameOverlay. Two rows of four cells — last cell
        // is a Spacer so the seven toggles keep uniform widths across both
        // rows. Labels are abbreviated to fit the ~74dp cell.
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            BubbleGridRow {
                ToggleBubble(stringResource(R.string.perf_speedhack_mtvu), s.mtvu, modifier = Modifier.weight(1f)) {
                    apply(s.copy(mtvu = it))
                }
                ToggleBubble(stringResource(R.string.perf_speedhack_instant_vu1), s.vu1Instant, modifier = Modifier.weight(1f)) {
                    apply(s.copy(vu1Instant = it))
                }
                ToggleBubble(stringResource(R.string.perf_speedhack_vu_flag_hack), s.vuFlagHack, modifier = Modifier.weight(1f)) {
                    apply(s.copy(vuFlagHack = it))
                }
                ToggleBubble(stringResource(R.string.perf_speedhack_fast_cdvd), s.fastCDVD, modifier = Modifier.weight(1f)) {
                    apply(s.copy(fastCDVD = it))
                }
            }
            BubbleGridRow {
                ToggleBubble(stringResource(R.string.perf_speedhack_intc_stat), s.intcStat, modifier = Modifier.weight(1f)) {
                    apply(s.copy(intcStat = it))
                }
                ToggleBubble(stringResource(R.string.perf_speedhack_wait_loop), s.waitLoop, modifier = Modifier.weight(1f)) {
                    apply(s.copy(waitLoop = it))
                }
                // Frame Limiter lives on the Play tab's quick toggles — no need
                // to duplicate it here.
                // ARMSX2 perf-knob: A/B disable for the arm64 VU1 JIT
                // NEON peephole fusions. Default on; flip off to confirm
                // whether a per-game regression traces back to our
                // matrix-transform / cross-product fusion peepholes.
                // Block recompilation is not invalidated on toggle —
                // already-compiled blocks keep their fused path; new
                // blocks pick up the new gate. Restart the game for a
                // clean A/B (or hit the recompiler tab to flip a CPU off
                // then on, which forces a cache rebuild).
                ToggleBubble(stringResource(R.string.perf_speedhack_vu_neon_fusions), s.vuNeonFusions, modifier = Modifier.weight(1f)) {
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
                ToggleBubble(stringResource(R.string.perf_speedhack_skip_vu_stall_sim), s.vuSkipStallSim, modifier = Modifier.weight(1f)) {
                    apply(s.copy(vuSkipStallSim = it))
                }
                ToggleBubble(stringResource(R.string.perf_speedhack_defer_vu_writes), s.vuDeferredWrites, modifier = Modifier.weight(1f)) {
                    apply(s.copy(vuDeferredWrites = it))
                }
                ToggleBubble(stringResource(R.string.perf_speedhack_skip_dupe_frames), s.skipDuplicateFrames, modifier = Modifier.weight(1f)) {
                    apply(s.copy(skipDuplicateFrames = it))
                }
                Spacer(Modifier.weight(1f))
            }
        }
    }
}
