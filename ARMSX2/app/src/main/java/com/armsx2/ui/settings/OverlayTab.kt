package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import com.armsx2.R
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.config.Settings
import com.armsx2.ui.InGameOverlay

/**
 * Performance Overlay element toggles. Lets the user show/hide individual
 * parts of the on-screen stats overlay (the master OSD pill on the Play tab
 * is still the quick all-on/all-off switch).
 *
 * The GPU toggle is special: turning it off also stops the GPU timing
 * queries (timestamp queries + per-frame readback have real overhead), so
 * it's a genuine performance lever, not just a display option — see GS.cpp
 * SetGPUTimingEnabled. Each toggle persists to base AND applies live via the
 * native osdShow* setters (see [InGameOverlay.applySafeLiveDelta]).
 */
@Composable
fun OverlayTab(state: MutableState<Settings>) {
    val s = state.value
    val scroll = remember { ScrollState(0) }
    ControllerAutoScroll(scroll)

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scroll),
    ) {
        Text(
            stringResource(R.string.overlay_section_help),
            color = Color(0xFFB0B0B0),
            fontSize = 11.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )

        ToggleRow(stringResource(R.string.overlay_show_gpu), s.osdShowGpu) {
            apply(s.copy(osdShowGpu = it))
        }
        SettingsDivider()
        ToggleRow(stringResource(R.string.overlay_show_cpu), s.osdShowCpu) { apply(s.copy(osdShowCpu = it)) }
        SettingsDivider()
        ToggleRow("FPS", s.osdShowFps) { apply(s.copy(osdShowFps = it)) }
        SettingsDivider()
        ToggleRow(stringResource(R.string.overlay_show_vps), s.osdShowVps) { apply(s.copy(osdShowVps = it)) }
        SettingsDivider()
        ToggleRow("Emulation speed %", s.osdShowSpeed) { apply(s.copy(osdShowSpeed = it)) }
        SettingsDivider()
        ToggleRow(stringResource(R.string.overlay_show_internal_res), s.osdShowResolution) { apply(s.copy(osdShowResolution = it)) }
        SettingsDivider()
        ToggleRow(stringResource(R.string.overlay_show_gs_stats), s.osdShowGsStats) { apply(s.copy(osdShowGsStats = it)) }
        SettingsDivider()
        ToggleRow(stringResource(R.string.overlay_show_frame_times), s.osdShowFrameTimes) { apply(s.copy(osdShowFrameTimes = it)) }
    }
}
