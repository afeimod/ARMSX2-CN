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
 * SPU2 audio output settings. Volume + mute apply live to the open audio
 * stream (NativeApp.setAudioVolume / setAudioMuted) and persist via ConfigStore.
 */
@Composable
fun AudioTab(state: MutableState<Settings>) {
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
            stringResource(R.string.audio_section_title),
            color = Color(0xFFB0B0B0),
            fontSize = 11.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )
        IntSliderRow(
            label = stringResource(R.string.audio_volume),
            value = s.audioVolume.coerceIn(0, 100),
            min = 0,
            max = 100,
            valueFormatter = { "$it%" },
            onChange = { apply(s.copy(audioVolume = it)) },
        )
        SettingsDivider()
        ToggleRow(stringResource(R.string.audio_mute), s.audioMuted) { apply(s.copy(audioMuted = it)) }
        SettingsDivider()
        ToggleRow(
            stringResource(R.string.audio_time_stretch),
            s.audioTimeStretch,
            description = stringResource(R.string.audio_time_stretch_help),
        ) { apply(s.copy(audioTimeStretch = it)) }
        SettingsDivider()
        IntSliderRow(
            label = stringResource(R.string.audio_buffer),
            value = s.audioBufferMs.coerceIn(20, 200),
            min = 20,
            max = 200,
            description = stringResource(R.string.audio_buffer_help),
            valueFormatter = { "$it ms" },
            onChange = { apply(s.copy(audioBufferMs = it)) },
        )
        SettingsDivider()
        IntSliderRow(
            label = stringResource(R.string.audio_output_latency),
            value = s.audioOutputLatencyMs.coerceIn(5, 100),
            min = 5,
            max = 100,
            description = stringResource(R.string.audio_output_latency_help),
            valueFormatter = { "$it ms" },
            onChange = { apply(s.copy(audioOutputLatencyMs = it)) },
        )
        SettingsDivider()
        IntSliderRow(
            label = stringResource(R.string.audio_ff_volume),
            value = s.audioFastForwardVolume.coerceIn(0, 100),
            min = 0,
            max = 100,
            description = stringResource(R.string.audio_ff_volume_help),
            valueFormatter = { "$it%" },
            onChange = { apply(s.copy(audioFastForwardVolume = it)) },
        )
    }
}
