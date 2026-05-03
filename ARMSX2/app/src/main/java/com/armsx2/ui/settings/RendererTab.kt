package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import com.armsx2.Main
import com.armsx2.config.ConfigStore
import com.armsx2.config.Settings
import kr.co.iefriends.pcsx2.NativeApp

/**
 * Renderer section of the in-game settings overlay.
 *
 * Most fields write into [Settings] via [ConfigStore]. Upscale is the
 * one outlier — it has its own dedicated `Main.upscale` state that's
 * also consumed by `Main.applyRendererPrefs` and the setup wizard, so
 * the slider mutates that state directly + calls
 * `NativeApp.renderUpscalemultiplier` for live-apply, mirroring the
 * existing toolbar behavior.
 */
@Composable
fun RendererTab(state: MutableState<Settings>) {
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
            label = "Upscale",
            value = Main.upscale.value,
            min = 1,
            max = 5,
            valueFormatter = { mult -> "${mult}x  ${640 * mult}×${448 * mult}" },
            onChange = { mult ->
                Main.upscale.value = mult
                Main.prefs.edit().putInt("upscale", mult).apply()
                NativeApp.renderUpscalemultiplier(mult.toFloat())
            },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Texture Filtering",
            options = listOf("Nearest", "Forced", "PS2", "Sprite"),
            selectedIndex = s.textureFiltering.coerceIn(0, 3),
            onChange = { apply(s.copy(textureFiltering = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Texture Preloading",
            options = listOf("Off", "Partial", "Full"),
            selectedIndex = s.texturePreloading.coerceIn(0, 2),
            onChange = { apply(s.copy(texturePreloading = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Blending Accuracy",
            options = listOf("Min", "Basic", "Med", "High", "Full", "Max"),
            selectedIndex = s.accurateBlendingUnit.coerceIn(0, 5),
            onChange = { apply(s.copy(accurateBlendingUnit = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Auto Flush",
            options = listOf("Off", "Sprites", "On"),
            selectedIndex = s.autoFlush.coerceIn(0, 2),
            onChange = { apply(s.copy(autoFlush = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Half-Pixel Offset",
            options = listOf("Off", "Normal", "Special", "Aggr.", "Native", "NW-Tex"),
            selectedIndex = s.halfPixelOffset.coerceIn(0, 5),
            onChange = { apply(s.copy(halfPixelOffset = it)) },
        )
        SettingsDivider()
        ToggleRow("HW Mipmapping", s.hwMipmap) {
            apply(s.copy(hwMipmap = it))
        }
        SettingsDivider()
        // TriFilter is signed (-1 = Auto). Map enum range onto 0..3.
        val triLabels = listOf("Auto", "Off", "PS2", "Forced")
        val triIdx = (s.triFilter + 1).coerceIn(0, 3)
        SegmentedRow(
            label = "Trilinear",
            options = triLabels,
            selectedIndex = triIdx,
            onChange = { apply(s.copy(triFilter = it - 1)) },
        )
        SettingsDivider()
        val anisoLabels = listOf("Off", "2x", "4x", "8x", "16x")
        val anisoVals = listOf(0, 2, 4, 8, 16)
        val anisoIdx = anisoVals.indexOf(s.maxAnisotropy).coerceAtLeast(0)
        SegmentedRow(
            label = "Anisotropic",
            options = anisoLabels,
            selectedIndex = anisoIdx,
            onChange = { apply(s.copy(maxAnisotropy = anisoVals[it])) },
        )
    }
}
