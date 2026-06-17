package com.armsx2.ui.settings

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.ScrollState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.documentfile.provider.DocumentFile
import com.armsx2.Main
import com.armsx2.config.Settings
import com.armsx2.ui.Colors
import com.armsx2.ui.InGameOverlay
import java.io.File

/**
 * Patch / cheat toggles + a PNACH importer.
 *
 * Widescreen / no-interlacing come from the bundled patch database (just toggle
 * them). User cheats are `.pnach` files dropped into <dataRoot>/cheats/ — import
 * them here, enable "Cheats (PNACH)", and restart the game. The .pnach must be
 * named to match the disc (e.g. `SLUS-12345_A1B2C3D4.pnach`) for emucore to pick
 * it up. All patch options inject at boot, so changes apply on game restart.
 */
@Composable
fun PatchesTab(state: MutableState<Settings>) {
    val s = state.value
    val context = LocalContext.current
    val scroll = remember { ScrollState(0) }

    val cheatsDir = remember { File(Main.assetCopyRoot(context), "cheats").apply { mkdirs() } }
    fun listPnach(): List<String> =
        cheatsDir.listFiles()
            ?.filter { it.isFile && it.name.endsWith(".pnach", ignoreCase = true) }
            ?.map { it.name }
            ?.sorted()
            ?: emptyList()
    var pnachFiles: List<String> by remember { mutableStateOf(listPnach()) }
    fun refresh() { pnachFiles = listPnach() }

    fun apply(updated: Settings) = InGameOverlay.saveSettings(updated)

    val importLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        val name = DocumentFile.fromSingleUri(context, uri)?.name
            ?.takeIf { it.isNotEmpty() }
            ?: "imported.pnach"
        // Force a .pnach extension so it's discoverable by emucore.
        val outName = if (name.endsWith(".pnach", ignoreCase = true)) name else "$name.pnach"
        runCatching {
            context.contentResolver.openInputStream(uri)?.use { ins ->
                File(cheatsDir, outName).outputStream().use { outs -> ins.copyTo(outs) }
            }
        }
        refresh()
    }

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .verticalScroll(scroll),
    ) {
        Text(
            "Patches apply at boot — restart the game after changing these.",
            color = Color(0xFFB0B0B0),
            fontSize = 11.sp,
            modifier = Modifier.padding(bottom = 8.dp),
        )
        ToggleRow("Enable Patches", s.enablePatches) { apply(s.copy(enablePatches = it)) }
        SettingsDivider()
        ToggleRow("Widescreen (16:9) Patches", s.enableWideScreenPatches) {
            apply(s.copy(enableWideScreenPatches = it))
        }
        SettingsDivider()
        ToggleRow("No-Interlacing Patches", s.enableNoInterlacingPatches) {
            apply(s.copy(enableNoInterlacingPatches = it))
        }
        SettingsDivider()
        ToggleRow("Cheats (PNACH)", s.enableCheats) { apply(s.copy(enableCheats = it)) }
        SettingsDivider()

        // ---- PNACH importer ----
        Text(
            "Cheat files (.pnach)",
            color = Color.White,
            fontSize = 13.sp,
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.padding(top = 6.dp, bottom = 2.dp),
        )
        Text(
            "Name must match the disc, e.g. SLUS-12345_A1B2C3D4.pnach",
            color = Color(0xFF8C8C8C),
            fontSize = 10.sp,
            modifier = Modifier.padding(bottom = 4.dp),
        )
        Box(
            Modifier
                .fillMaxWidth()
                .height(36.dp)
                .background(rowAura())
                .clickable { importLauncher.launch(arrayOf("*/*")) }
                .padding(horizontal = 8.dp),
            contentAlignment = Alignment.CenterStart,
        ) {
            Text("+ Import .pnach file", color = Colors.pasx2_blue, fontSize = 13.sp, fontWeight = FontWeight.Bold)
        }
        if (pnachFiles.isEmpty()) {
            Text(
                "No cheat files imported yet.",
                color = Color(0xFF8C8C8C),
                fontSize = 11.sp,
                modifier = Modifier.padding(vertical = 4.dp, horizontal = 4.dp),
            )
        } else {
            pnachFiles.forEach { fileName ->
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(30.dp)
                        .padding(horizontal = 4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        fileName,
                        color = Color(0xFFCCCCCC),
                        fontSize = 11.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f),
                    )
                    Text(
                        "Delete",
                        color = Color(0xFFFF6B6B),
                        fontSize = 11.sp,
                        fontWeight = FontWeight.Bold,
                        modifier = Modifier
                            .clickable {
                                runCatching { File(cheatsDir, fileName).delete() }
                                refresh()
                            }
                            .padding(start = 8.dp),
                    )
                }
            }
        }
    }
}
