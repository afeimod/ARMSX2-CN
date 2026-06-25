package com.armsx2.ui.settings

import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.res.stringResource
import com.armsx2.config.Settings
import com.armsx2.R
import com.armsx2.ui.Colors
import com.armsx2.ui.InGameOverlay

/**
 * Hardware / upscaling compatibility fixes — the PCSX2 "Hardware Fixes" and
 * "Upscaling Fixes" panels. Split out of [RendererTab] so Render keeps only
 * core quality/display settings.
 *
 * Every row writes into [Settings] via [InGameOverlay.saveSettings]; on a
 * running VM that reconfigures the GS live (Settings.applyGsLive → native
 * applyGSSettingsLive) so changes show without a restart. Note PCSX2 masks
 * upscaling hacks at native (1x) resolution and masks every UserHacks_* key
 * unless at least one fix is enabled — both are intentional parity behaviours.
 */
@Composable
fun FixesTab(state: MutableState<Settings>) {
    val s = state.value
    val scroll = remember { ScrollState(0) }
    ControllerAutoScroll(scroll)

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scroll),
    ) {
        SectionHeader(stringResource(R.string.fixes_section_display))
        HelpText(
            stringResource(R.string.fixes_display_help),
            modifier = Modifier.padding(horizontal = 6.dp),
        )
        SettingsDivider()
        ToggleRow(
            stringResource(R.string.fixes_anti_blur),
            s.antiBlur,
            description = stringResource(R.string.fixes_anti_blur_help),,
        ) { apply(s.copy(antiBlur = it)) }
        SettingsDivider()
        ToggleRow(
            stringResource(R.string.fixes_screen_offsets),
            s.screenOffsets,
            description = stringResource(R.string.fixes_screen_offsets_help),,
        ) { apply(s.copy(screenOffsets = it)) }
        SettingsDivider()
        ToggleRow(
            stringResource(R.string.fixes_show_overscan),
            s.showOverscan,
            description = stringResource(R.string.fixes_show_overscan_help),,
        ) { apply(s.copy(showOverscan = it)) }
        SettingsDivider()
        ToggleRow(
            stringResource(R.string.fixes_disable_interlace_offset),
            s.disableInterlaceOffset,
            description = stringResource(R.string.fixes_disable_interlace_offset_help),,
        ) { apply(s.copy(disableInterlaceOffset = it)) }
        SettingsDivider()
        ToggleRow(
            stringResource(R.string.fixes_sync_to_host_refresh),
            s.syncToHostRefresh,
            description = stringResource(R.string.fixes_sync_to_host_refresh_help),,
        ) { apply(s.copy(syncToHostRefresh = it)) }
        SettingsDivider()
        ToggleRow(
            stringResource(R.string.fixes_disable_framebuffer_fetch),
            s.disableFramebufferFetch,
            description = stringResource(R.string.fixes_disable_framebuffer_fetch_help),,
        ) { apply(s.copy(disableFramebufferFetch = it)) }
        SettingsDivider()
        SegmentedRow(
            label = "Override Texture Barriers",
            options = listOf("Auto", "Off", "On"),
            selectedIndex = (s.overrideTextureBarriers + 1).coerceIn(0, 2),
            description = stringResource(R.string.fixes_override_texture_barriers_help),
            onChange = { apply(s.copy(overrideTextureBarriers = it - 1)) },
        )
        SettingsDivider()
        ToggleRow(
            stringResource(R.string.fixes_hw_accurate_alpha_test),
            s.hwAccurateAlphaTest,
            description = stringResource(R.string.fixes_hw_accurate_alpha_test_help),,
        ) { apply(s.copy(hwAccurateAlphaTest = it)) }
        SettingsDivider()
        ToggleRow(
            stringResource(R.string.fixes_disable_vertex_shader_expand),
            s.disableVertexShaderExpand,
            description = stringResource(R.string.fixes_disable_vertex_shader_expand_help),,
        ) { apply(s.copy(disableVertexShaderExpand = it)) }
        SettingsDivider()
        ToggleRow(
            stringResource(R.string.fixes_use_blit_swap_chain),
            s.useBlitSwapChain,
            description = stringResource(R.string.fixes_use_blit_swap_chain_help),,
        ) { apply(s.copy(useBlitSwapChain = it)) }
        SettingsDivider()
        ToggleRow(
            stringResource(R.string.fixes_disable_shader_cache),
            s.disableShaderCache,
            description = stringResource(R.string.fixes_disable_shader_cache_help),,
        ) { apply(s.copy(disableShaderCache = it)) }
        SettingsDivider()
        ToggleRow(
            stringResource(R.string.fixes_integer_scaling),
            s.integerScaling,
            description = stringResource(R.string.fixes_integer_scaling_help),,
        ) { apply(s.copy(integerScaling = it)) }
        SettingsDivider()
        SegmentedRow(
            label = "Dithering",
            options = listOf("Off", "Scaled", "Unscaled"),
            selectedIndex = s.dithering.coerceIn(0, 2),
            description = stringResource(R.string.fixes_dithering_help),
            onChange = { apply(s.copy(dithering = it)) },
        )
        SettingsDivider()
        IntSliderRow(
            label = "Vsync Queue Size",
            value = s.vsyncQueueSize.coerceIn(0, 3),
            min = 0,
            max = 3,
            description = stringResource(R.string.fixes_vsync_queue_size_help),
            onChange = { apply(s.copy(vsyncQueueSize = it)) },
        )

        SectionHeader(stringResource(R.string.fixes_section_upscaling))
        HelpText(
            stringResource(R.string.fixes_upscaling_help),
            modifier = Modifier.padding(horizontal = 6.dp),
        )
        SettingsDivider()
        SegmentedRow(
            label = "Upscaling Fixes",
            options = listOf("Off", "Normal", "Aggr.", "Normal+", "Aggr.+"),
            selectedIndex = s.nativeScaling.coerceIn(0, 4),
            description = stringResource(R.string.fixes_upscaling_help),
            onChange = { apply(s.copy(nativeScaling = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Half-Pixel Offset",
            options = listOf("Off", "Normal", "Special", "Aggr.", "Native", "NW-Tex"),
            selectedIndex = s.halfPixelOffset.coerceIn(0, 5),
            description = stringResource(R.string.fixes_half_pixel_offset_help),
            onChange = { apply(s.copy(halfPixelOffset = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Round Sprite",
            options = listOf("Off", "Half", "Full"),
            selectedIndex = s.roundSprite.coerceIn(0, 2),
            description = stringResource(R.string.fixes_round_sprite_help),
            onChange = { apply(s.copy(roundSprite = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Bilinear Dirty",
            options = listOf("Off", "Normal", "Half", "Forced"),
            selectedIndex = s.bilinearUpscale.coerceIn(0, 3),
            description = stringResource(R.string.fixes_bilinear_filter_help),
            onChange = { apply(s.copy(bilinearUpscale = it)) },
        )
        SettingsDivider()
        ToggleRow(
            "Align Sprite",
            s.alignSprite,
            description = stringResource(R.string.fixes_vertical_stretch_help),
        ) { apply(s.copy(alignSprite = it)) }
        SettingsDivider()
        ToggleRow(
            "Merge Sprite",
            s.mergeSprite,
            description = stringResource(R.string.fixes_merge_sprites_help),
        ) { apply(s.copy(mergeSprite = it)) }
        SettingsDivider()
        ToggleRow(
            "Wild Arms Offset",
            s.forceEvenSpritePosition,
            description = stringResource(R.string.fixes_force_even_sprite_positions_help),
        ) { apply(s.copy(forceEvenSpritePosition = it)) }
        SettingsDivider()
        ToggleRow(
            "Unscaled Palette Draw",
            s.unscaledPaletteDraw,
            description = stringResource(R.string.fixes_palette_texture_help),
        ) { apply(s.copy(unscaledPaletteDraw = it)) }
        SettingsDivider()
        IntSliderRow(
            label = "Texture Offset X",
            value = s.textureOffsetX.coerceIn(0, 1000),
            min = 0,
            max = 1000,
            description = stringResource(R.string.fixes_texture_offset_x_help),
            onChange = { apply(s.copy(textureOffsetX = it)) },
        )
        SettingsDivider()
        IntSliderRow(
            label = "Texture Offset Y",
            value = s.textureOffsetY.coerceIn(0, 1000),
            min = 0,
            max = 1000,
            description = stringResource(R.string.fixes_texture_offset_y_help),
            onChange = { apply(s.copy(textureOffsetY = it)) },
        )

        SectionHeader(stringResource(R.string.fixes_section_hardware))
        HelpText(
            "Manual renderer hacks. The master toggle auto-enables when any fix is " +
                "set. Leave these off unless fixing a specific visual issue.",
            modifier = Modifier.padding(horizontal = 6.dp),
        )
        SettingsDivider()
        ToggleRow(
            "Manual Hardware Fixes",
            s.manualUserHacks,
            description = stringResource(R.string.fixes_force_user_hacks_help),
        ) { apply(s.copy(manualUserHacks = it)) }
        SettingsDivider()
        SegmentedRow(
            label = "Auto Flush",
            options = listOf("Off", "Sprites", "On"),
            selectedIndex = s.autoFlush.coerceIn(0, 2),
            description = stringResource(R.string.fixes_alpha_prite_help),
            onChange = { apply(s.copy(autoFlush = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Texture Inside RT",
            options = listOf("Off", "Inside", "Merge"),
            selectedIndex = s.textureInsideRt.coerceIn(0, 2),
            description = stringResource(R.string.fixes_rt_sampling_help),
            onChange = { apply(s.copy(textureInsideRt = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "GPU Target CLUT",
            options = listOf("Off", "Inside", "Forced"),
            selectedIndex = s.gpuTargetClut.coerceIn(0, 2),
            description = stringResource(R.string.fixes_clut_render_help),
            onChange = { apply(s.copy(gpuTargetClut = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "CPU Sprite BW",
            options = listOf("Off", "64", "128", "256"),
            selectedIndex = s.cpuSpriteRenderBw.coerceIn(0, 3),
            description = stringResource(R.string.fixes_sprite_bandwidth_help),
            onChange = { apply(s.copy(cpuSpriteRenderBw = it)) },
        )
        SettingsDivider()
        SegmentedGridRow(
            label = "CPU Sprite Render",
            options = listOf("Off", "Sprite", "Triangle", "Aggressive", "Full", "Max"),
            selectedIndex = s.cpuSpriteRenderLevel.coerceIn(0, 5),
            columns = 3,
            description = stringResource(R.string.fixes_sprite_cpu_help),
            onChange = { apply(s.copy(cpuSpriteRenderLevel = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "CPU CLUT Render",
            options = listOf("Off", "Normal", "Aggr."),
            selectedIndex = s.cpuClutRender.coerceIn(0, 2),
            description = stringResource(R.string.fixes_cpu_clut_help),
            onChange = { apply(s.copy(cpuClutRender = it)) },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Limit 24-Bit Depth",
            options = listOf("Off", "Upper", "Lower"),
            selectedIndex = s.limit24BitDepth.coerceIn(0, 2),
            description = stringResource(R.string.fixes_depth_zfb_help),
            onChange = { apply(s.copy(limit24BitDepth = it)) },
        )
        SettingsDivider()
        ToggleRow(
            "GPU Palette Conversion",
            s.gpuPaletteConversion,
            description = stringResource(R.string.fixes_palette_conversion_help),
        ) { apply(s.copy(gpuPaletteConversion = it)) }
        SettingsDivider()
        ToggleRow(
            "CPU Framebuffer Conversion",
            s.cpuFramebufferConversion,
            description = stringResource(R.string.fixes_fb_conversion_help),
        ) { apply(s.copy(cpuFramebufferConversion = it)) }
        SettingsDivider()
        ToggleRow(
            "Read Targets When Closing",
            s.readTargetsWhenClosing,
            description = stringResource(R.string.fixes_flush_rt_after_draw_help),
        ) { apply(s.copy(readTargetsWhenClosing = it)) }
        SettingsDivider()
        ToggleRow(
            stringResource(R.string.fixes_preload_frame_data),
            s.preloadFrameData,
            description = stringResource(R.string.fixes_preload_frame_data_help),,
        ) { apply(s.copy(preloadFrameData = it)) }
        SettingsDivider()
        ToggleRow(
            "Estimate Texture Region",
            s.estimateTextureRegion,
            description = stringResource(R.string.fixes_texture_inside_rt_help),
        ) { apply(s.copy(estimateTextureRegion = it)) }
        SettingsDivider()
        ToggleRow(
            "Draw Buffering",
            s.drawBuffering,
            description = stringResource(R.string.fixes_command_buffer_help),
        ) { apply(s.copy(drawBuffering = it)) }
        SettingsDivider()
        ToggleRow(
            "Disable Depth Emulation",
            s.disableDepthEmulation,
            description = stringResource(R.string.fixes_disable_depth_conversion_help),
        ) { apply(s.copy(disableDepthEmulation = it)) }
        SettingsDivider()
        ToggleRow(
            "Disable Partial Invalidation",
            s.disablePartialInvalidation,
            description = stringResource(R.string.fixes_disable_invalidation_help),
        ) { apply(s.copy(disablePartialInvalidation = it)) }
        SettingsDivider()
        ToggleRow(
            "Disable Safe Features",
            s.disableSafeFeatures,
            description = "Turns off internal safe-feature workarounds. Advanced/diagnostic only.",
        ) { apply(s.copy(disableSafeFeatures = it)) }
        SettingsDivider()
        ToggleRow(
            "Disable Render Fixes",
            s.disableRenderFixes,
            description = "Disables automatic render fixes. Advanced/diagnostic only.",
        ) { apply(s.copy(disableRenderFixes = it)) }
        SettingsDivider()
        IntSliderRow(
            label = "Skip Draw Start",
            value = s.skipDrawStart.coerceIn(0, 5000),
            min = 0,
            max = 5000,
            description = "First draw call to skip (UserHacks_SkipDraw). 0 = off. Advanced.",
            onChange = { apply(s.copy(skipDrawStart = it)) },
        )
        SettingsDivider()
        IntSliderRow(
            label = "Skip Draw End",
            value = s.skipDrawEnd.coerceIn(0, 5000),
            min = 0,
            max = 5000,
            description = "Last draw call to skip. 0 = off. Advanced.",
            onChange = { apply(s.copy(skipDrawEnd = it)) },
        )
        SettingsDivider()
        ToggleRow(
            "Spin GPU For Readbacks",
            s.spinGpuReadbacks,
            description = stringResource(R.string.fixes_gpu_busy_wait_help),
        ) { apply(s.copy(spinGpuReadbacks = it)) }
        SettingsDivider()
        ToggleRow(
            "Spin CPU For Readbacks",
            s.spinCpuReadbacks,
            description = stringResource(R.string.fixes_cpu_busy_wait_help),
        ) { apply(s.copy(spinCpuReadbacks = it)) }

        SectionHeader("Software Renderer")
        HelpText(
            "Apply when the Software renderer is selected.",
            modifier = Modifier.padding(horizontal = 6.dp),
        )
        SettingsDivider()
        ToggleRow(
            "Auto-Flush (SW)",
            s.autoFlushSw,
            description = stringResource(R.string.fixes_sw_auto_flush_help),
        ) { apply(s.copy(autoFlushSw = it)) }
        SettingsDivider()
        ToggleRow(
            "Mipmapping (SW)",
            s.mipmapSw,
            description = stringResource(R.string.fixes_sw_mipmap_help),
        ) { apply(s.copy(mipmapSw = it)) }
        SettingsDivider()
        IntSliderRow(
            label = "SW Rendering Threads",
            value = s.swThreads.coerceIn(0, 10),
            min = 0,
            max = 10,
            description = stringResource(R.string.fixes_sw_threads_help),
            onChange = { apply(s.copy(swThreads = it)) },
        )
        SettingsDivider()
        IntSliderRow(
            label = "SW Thread Tile Height",
            value = s.swThreadsHeight.coerceIn(0, 8),
            min = 0,
            max = 8,
            description = stringResource(R.string.fixes_sw_tile_height_help),
            onChange = { apply(s.copy(swThreadsHeight = it)) },
        )
        Spacer(Modifier.height(8.dp))
    }
}

@Composable
private fun SectionHeader(title: String) {
    Spacer(Modifier.height(8.dp))
    Text(
        title,
        color = Colors.pasx2_blue,
        fontSize = 12.sp,
        fontWeight = FontWeight.Bold,
        modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
    )
}
