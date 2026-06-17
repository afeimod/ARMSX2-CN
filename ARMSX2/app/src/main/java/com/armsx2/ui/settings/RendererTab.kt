package com.armsx2.ui.settings

import android.content.Context
import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts.OpenDocumentTree
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.documentfile.provider.DocumentFile
import com.armsx2.Main
import com.armsx2.config.Settings
import com.armsx2.ui.Colors
import com.armsx2.ui.InGameOverlay
import kr.co.iefriends.pcsx2.NativeApp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/**
 * Renderer section of the in-game settings overlay.
 *
 * Most fields write into [Settings] via [InGameOverlay.saveSettings],
 * which honors the overlay's scope toggle (Global / Game). Upscale is
 * the one outlier — it has its own dedicated `Main.upscale` state that's
 * also consumed by `Main.applyRendererPrefs` and the setup wizard. Upscale
 * uses a narrow native GS helper so it can visibly apply while a game is live
 * without running the full settings commit path.
 */
@Composable
fun RendererTab(state: MutableState<Settings>) {
    val s = state.value
    val scroll = remember { ScrollState(0) }

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scroll),
    ) {
        // Graphics API (OpenGL / Vulkan) + Vulkan custom-driver picker. Ported
        // from the removed first-run setup renderer page into settings.
        RendererBackendSection()
        SettingsDivider()
        IntSliderRow(
            label = "Upscale",
            value = Main.upscale.value,
            min = 1,
            max = 5,
            valueFormatter = { mult -> "${mult}x  ${640 * mult}×${448 * mult}" },
            onChange = { mult ->
                if (Main.upscale.value != mult) {
                    Main.upscale.value = mult
                    Main.prefs.edit().putInt("upscale", mult).apply()
                    NativeApp.renderUpscalemultiplier(mult.toFloat())
                }
            },
        )
        SettingsDivider()
        SegmentedRow(
            label = "Display Mode",
            options = listOf("Stretch", "Auto", "4:3", "16:9", "10:7"),
            selectedIndex = s.aspectRatio.coerceIn(0, 4),
            onChange = { apply(s.copy(aspectRatio = it)) },
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
        ToggleRow("Load Texture Packs", s.loadTextureReplacements) {
            apply(s.copy(loadTextureReplacements = it))
        }
        SettingsDivider()
        ToggleRow("Async Texture Loading", s.loadTextureReplacementsAsync) {
            apply(s.copy(loadTextureReplacementsAsync = it))
        }
        SettingsDivider()
        ToggleRow("Precache Texture Packs", s.precacheTextureReplacements) {
            apply(s.copy(precacheTextureReplacements = it))
        }
        SettingsDivider()
        TexturePackImportRow()
        SettingsDivider()
        ToggleRow("Dump Replaceable Textures", s.dumpReplaceableTextures) {
            apply(s.copy(dumpReplaceableTextures = it))
        }
        SettingsDivider()
        ToggleRow("Texture Pack OSD", s.osdShowTextureReplacements) {
            apply(s.copy(osdShowTextureReplacements = it))
        }
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
        SettingsDivider()
        // GPU profile override. Auto resolves at device init via
        // GpuProfileDetector::Resolve (vendor strings + Android ro.soc.*
        // hints). Mali uses ARM_shader_framebuffer_fetch over texture
        // barriers; Adreno uses the EXT fetch / generic path; PowerVR
        // (Imagination) uses EXT/PLS like Adreno but is its own tile-based
        // GPU family. Changing requires a renderer restart — CheckFeatures
        // runs once at device init, so we kick Main.restart() the same way
        // RestartButton does.
        SegmentedRow(
            label = "GPU Profile",
            options = listOf("Auto", "Mali", "Adreno", "PowerVR"),
            selectedIndex = s.gpuProfile.coerceIn(0, 3),
            onChange = {
                apply(s.copy(gpuProfile = it))
            },
        )
    }
}

@Composable
private fun TexturePackImportRow() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val status = remember { mutableStateOf("") }
    val launcher = rememberLauncherForActivityResult(OpenDocumentTree()) { uri: Uri? ->
        val serial = activeTextureSerial()
        if (uri == null) {
            return@rememberLauncherForActivityResult
        } else if (serial == null) {
            Toast.makeText(context, "Boot a game before importing its texture pack.", Toast.LENGTH_LONG).show()
        } else {
            scope.launch(Dispatchers.IO) {
                val copied = runCatching { importTexturePack(context, uri, serial) }.getOrDefault(-1)
                withContext(Dispatchers.Main) {
                    val msg = if (copied >= 0)
                        "Imported $copied texture files for $serial."
                    else
                        "Texture pack import failed."
                    status.value = msg
                    Toast.makeText(context, msg, Toast.LENGTH_LONG).show()
                }
            }
        }
    }

    Box(
        Modifier
            .fillMaxWidth()
            .background(rowAura())
            .clickable { launcher.launch(null) }
            .padding(horizontal = 6.dp, vertical = 5.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Column {
            Text(
                "Import Texture Pack",
                color = Color.White,
                fontSize = 13.sp,
                fontWeight = FontWeight.SemiBold,
            )
            Spacer(Modifier.height(2.dp))
            Text(
                status.value.ifEmpty {
                    activeTextureSerial()?.let { "Copies into textures/$it/replacements" }
                        ?: "Boot a game first so ARMSX2 knows the serial."
                },
                color = Colors.pasx2_blue,
                fontSize = 10.sp,
                fontWeight = FontWeight.Bold,
            )
        }
    }
}

private fun activeTextureSerial(): String? {
    return Main.currentGame.value?.serial?.takeIf { it.isNotBlank() }
        ?: runCatching { NativeApp.getGameSerial() }.getOrNull()?.takeIf { it.isNotBlank() }
}

private fun importTexturePack(context: Context, uri: Uri, serial: String): Int {
    val root = DocumentFile.fromTreeUri(context, uri) ?: return -1
    val source = root.findFile("replacements")?.takeIf { it.isDirectory } ?: root
    val dest = File(Main.assetCopyRoot(context), "textures/$serial/replacements")
    if (!dest.exists() && !dest.mkdirs())
        return -1
    return copyDocumentTree(context, source, dest)
}

private fun copyDocumentTree(context: Context, source: DocumentFile, dest: File): Int {
    var copied = 0
    for (child in source.listFiles()) {
        val name = child.name ?: continue
        if (child.isDirectory) {
            val childDest = File(dest, name)
            if (!childDest.exists())
                childDest.mkdirs()
            copied += copyDocumentTree(context, child, childDest)
        } else if (child.isFile) {
            context.contentResolver.openInputStream(child.uri)?.use { input ->
                File(dest, name).outputStream().use { output ->
                    input.copyTo(output)
                }
            } ?: continue
            copied++
        }
    }
    return copied
}
