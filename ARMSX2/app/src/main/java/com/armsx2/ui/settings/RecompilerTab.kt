package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.material3.Text
import com.armsx2.config.Settings
import com.armsx2.ui.InGameOverlay

/**
 * Recompiler section of the in-game settings overlay.
 *
 * Disables here drop the corresponding CPU/COP onto its interpreter
 * fallback. They mirror EmuCore/CPU/Recompiler keys (see Settings field
 * comments for the exact `<section>/<key>`). Toggling these on a running
 * VM swaps the dispatch pointer via VMManager::ApplySettings's
 * CpusChanged path; existing JIT block caches are flushed automatically,
 * but mid-frame switching is still inherently risky — interpreter speed
 * is much lower so most scenes will tank performance, and a few games
 * have known interpreter divergences. Treat this tab as a debug tool.
 */
@Composable
fun RecompilerTab(state: MutableState<Settings>) {
    val s = state.value
    val scroll = remember { ScrollState(0) }

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scroll),
    ) {
        Text(
            "Disabling a recompiler drops to interpreter — debug only, expect a heavy slowdown.",
            color = Color(0xFFB0B0B0),
            fontSize = 11.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )
        // 4-wide toggle grid, matching the Playing-Now + Performance look.
        // Row 1: the four CPU/COP recompilers (original-vs-interpreter).
        // Row 2: Fastmem + spacers.
        // Row 3: per-CPU A/B toggle between original arm64 backend (off, default)
        //        and the macOS-port arm64 backend (on). VM restart required.
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            BubbleGridRow {
                ToggleBubble("EE (R5900)", s.recEE, modifier = Modifier.weight(1f)) {
                    apply(s.copy(recEE = it))
                }
                ToggleBubble("IOP (R3000)", s.recIOP, modifier = Modifier.weight(1f)) {
                    apply(s.copy(recIOP = it))
                }
                ToggleBubble("VU0", s.recVU0, modifier = Modifier.weight(1f)) {
                    apply(s.copy(recVU0 = it))
                }
                ToggleBubble("VU1", s.recVU1, modifier = Modifier.weight(1f)) {
                    apply(s.copy(recVU1 = it))
                }
            }
            BubbleGridRow {
                ToggleBubble("Fastmem", s.enableFastmem, modifier = Modifier.weight(1f)) {
                    apply(s.copy(enableFastmem = it))
                }
                Spacer(Modifier.weight(1f))
                Spacer(Modifier.weight(1f))
                Spacer(Modifier.weight(1f))
            }
            Text(
                "Mac backend A/B — flip individual CPUs to the macOS-port JIT to bisect regressions. VM restart applies.",
                color = Color(0xFFB0B0B0),
                fontSize = 11.sp,
                modifier = Modifier.padding(top = 8.dp, bottom = 4.dp),
            )
            BubbleGridRow {
                ToggleBubble("Mac EE", s.useMacEE, modifier = Modifier.weight(1f)) {
                    apply(s.copy(useMacEE = it))
                }
                ToggleBubble("Mac IOP", s.useMacIOP, modifier = Modifier.weight(1f)) {
                    apply(s.copy(useMacIOP = it))
                }
                ToggleBubble("Mac VU0", s.useMacVU0, modifier = Modifier.weight(1f)) {
                    apply(s.copy(useMacVU0 = it))
                }
                ToggleBubble("Mac VU1", s.useMacVU1, modifier = Modifier.weight(1f)) {
                    apply(s.copy(useMacVU1 = it))
                }
            }
            Text(
                "microVU pipeline-stall folding (port from mac) — replaces 17-32% of total CPU spent in vu1_TestFMAC* BLs with a compile-time inline Add. Default off until shadow-verify clean.",
                color = Color(0xFFB0B0B0),
                fontSize = 11.sp,
                modifier = Modifier.padding(top = 8.dp, bottom = 4.dp),
            )
            BubbleGridRow {
                ToggleBubble("VU1 Inline FMAC stall", s.vu1InlineFmacStall, modifier = Modifier.weight(1f)) {
                    apply(s.copy(vu1InlineFmacStall = it))
                }
                ToggleBubble("VU1 X-block pState", s.vu1CrossBlockPState, modifier = Modifier.weight(1f)) {
                    apply(s.copy(vu1CrossBlockPState = it))
                }
                ToggleBubble("VU1 Inline TestPipes", s.vu1InlineDrainTestPipes, modifier = Modifier.weight(1f)) {
                    apply(s.copy(vu1InlineDrainTestPipes = it))
                }
                Spacer(Modifier.weight(1f))
            }
        }
    }
}
