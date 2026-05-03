package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import com.armsx2.config.ConfigStore
import com.armsx2.config.Settings

/**
 * Performance section of the in-game settings overlay.
 *
 * Mutates the global [Settings] in [ConfigStore] on every change and
 * applies via [Settings.applyTo] — toggles take effect immediately on a
 * running VM. Every visible setting maps 1-1 onto an EmuCore key (see
 * Settings field comments for the exact `<section>/<key>`).
 *
 * Column + verticalScroll instead of LazyColumn so the tab can sit
 * inside the wrap-content RootTabs container without needing a hard
 * height bound. List is short (~9 rows) so non-lazy is fine.
 */
@Composable
fun PerformanceTab(state: MutableState<Settings>) {
    val s = state.value
    val scroll = remember { ScrollState(0) }

    fun apply(updated: Settings) {
        state.value = updated
        ConfigStore.saveGlobal(updated)
        updated.applyTo()
    }

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
            onChange = { apply(s.copy(eeCycleSkip = it)) },
        )
        SettingsDivider()
        ToggleRow("MTVU (Multi-Threaded VU1)", s.mtvu) {
            apply(s.copy(mtvu = it))
        }
        SettingsDivider()
        ToggleRow("Instant VU1", s.vu1Instant) {
            apply(s.copy(vu1Instant = it))
        }
        SettingsDivider()
        ToggleRow("VU Flag Hack", s.vuFlagHack) {
            apply(s.copy(vuFlagHack = it))
        }
        SettingsDivider()
        ToggleRow("Fast CDVD", s.fastCDVD) {
            apply(s.copy(fastCDVD = it))
        }
        SettingsDivider()
        ToggleRow("INTC Stat Hack", s.intcStat) {
            apply(s.copy(intcStat = it))
        }
        SettingsDivider()
        ToggleRow("Wait Loop Detection", s.waitLoop) {
            apply(s.copy(waitLoop = it))
        }
        SettingsDivider()
        ToggleRow("Frame Limiter", s.frameLimitEnable) {
            apply(s.copy(frameLimitEnable = it))
        }
    }
}
