package com.armsx2.ui

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
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
import com.armsx2.config.ConfigStore
import java.io.File
import java.io.FileOutputStream
import kr.co.iefriends.pcsx2.NativeApp

object MemoryCardManager {
    val visible = mutableStateOf(false)
    private val files = mutableStateListOf<File>()
    private val status = mutableStateOf<String?>(null)

    @Composable
    fun Render() {
        val context = LocalContext.current
        val fileLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.GetContent()
        ) { uri: Uri? ->
            if (uri != null) {
                importSlot1(context, uri)
                refresh(context)
            }
        }
        val folderLauncher = rememberLauncherForActivityResult(
            contract = ActivityResultContracts.OpenDocumentTree()
        ) { uri: Uri? ->
            if (uri != null) {
                importFolderCardSlot1(context, uri)
                refresh(context)
            }
        }

        LaunchedEffect(Unit) {
            refresh(context)
        }

        Box(
            Modifier
                .fillMaxSize()
                .background(Color.Black.copy(alpha = 0.58f)),
            contentAlignment = Alignment.Center,
        ) {
            Column(
                Modifier
                    .fillMaxWidth(0.78f)
                    .border(1.dp, Color(0xFF333333), RoundedCornerShape(12.dp))
                    .background(Colors.surface.value, RoundedCornerShape(12.dp))
                    .padding(18.dp),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(
                            "Memory Cards",
                            color = Color.White,
                            fontSize = 24.sp,
                            fontWeight = FontWeight.Bold,
                        )
                        Text(
                            memcardsDir(context).absolutePath,
                            color = Color(0xFFAAAAAA),
                            fontSize = 11.sp,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                    Button(
                        onClick = { visible.value = false },
                        colors = darkButtonColors(),
                        shape = RoundedCornerShape(8.dp),
                    ) {
                        Text("Close")
                    }
                }

                Spacer(Modifier.height(14.dp))

                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Button(
                        onClick = { fileLauncher.launch("*/*") },
                        colors = ps2ButtonColors(),
                        shape = RoundedCornerShape(8.dp),
                    ) {
                        Text("Import File")
                    }
                    Button(
                        onClick = { folderLauncher.launch(null) },
                        colors = ps2ButtonColors(),
                        shape = RoundedCornerShape(8.dp),
                    ) {
                        Text("Import Folder")
                    }
                    Button(
                        onClick = {
                            ensureDefaultSlots(context)
                            refresh(context)
                        },
                        colors = ps2ButtonColors(),
                        shape = RoundedCornerShape(8.dp),
                    ) {
                        Text("Use Default Slots")
                    }
                    Button(
                        onClick = { refresh(context) },
                        colors = darkButtonColors(),
                        shape = RoundedCornerShape(8.dp),
                    ) {
                        Text("Refresh")
                    }
                }

                status.value?.let {
                    Spacer(Modifier.height(10.dp))
                    Text(it, color = Color(0xFFBFD7FF), fontSize = 12.sp)
                }

                Spacer(Modifier.height(14.dp))

                if (files.isEmpty()) {
                    Box(
                        Modifier
                            .fillMaxWidth()
                            .height(96.dp)
                            .background(Color(0xFF151515), RoundedCornerShape(8.dp)),
                        contentAlignment = Alignment.Center,
                    ) {
                        Text(
                            "No memory-card files found yet.",
                            color = Color(0xFF999999),
                            fontSize = 13.sp,
                        )
                    }
                } else {
                    LazyColumn(
                        Modifier
                            .fillMaxWidth()
                            .height(160.dp)
                            .background(Color(0xFF151515), RoundedCornerShape(8.dp))
                            .padding(8.dp),
                    ) {
                        items(files, key = { it.absolutePath }) { file ->
                            Row(
                                Modifier
                                    .fillMaxWidth()
                                    .padding(vertical = 5.dp),
                                verticalAlignment = Alignment.CenterVertically,
                            ) {
                                Text(
                                    file.name,
                                    color = Color.White,
                                    fontSize = 13.sp,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                    modifier = Modifier.weight(1f),
                                )
                                Spacer(Modifier.width(8.dp))
                                Text(
                                    if (file.isDirectory) "Folder" else readableSize(file.length()),
                                    color = Color(0xFFAAAAAA),
                                    fontSize = 11.sp,
                                )
                                Spacer(Modifier.width(8.dp))
                                Button(
                                    onClick = { assignSlot(context, 1, file) },
                                    colors = darkButtonColors(),
                                    shape = RoundedCornerShape(6.dp),
                                    modifier = Modifier.height(32.dp),
                                ) {
                                    Text("Slot 1", fontSize = 11.sp)
                                }
                                Spacer(Modifier.width(6.dp))
                                Button(
                                    onClick = { assignSlot(context, 2, file) },
                                    colors = darkButtonColors(),
                                    shape = RoundedCornerShape(6.dp),
                                    modifier = Modifier.height(32.dp),
                                ) {
                                    Text("Slot 2", fontSize = 11.sp)
                                }
                            }
                        }
                    }
                }

                Spacer(Modifier.height(12.dp))
                Text(
                    "File imports are copied to Mcd001.ps2. Folder imports are copied as folder cards. Existing .ps2/.mcr files and folder cards can be assigned to either slot.",
                    color = Color(0xFF888888),
                    fontSize = 11.sp,
                )
            }
        }
    }

    fun refresh(context: Context) {
        val dir = memcardsDir(context)
        if (!dir.exists()) dir.mkdirs()
        files.clear()
        dir.listFiles()
            ?.filter { it.isFile || it.isDirectory }
            ?.sortedWith(compareBy<File> { !it.name.endsWith(".ps2", ignoreCase = true) }.thenBy { it.name.lowercase() })
            ?.let { files.addAll(it) }
    }

    private fun importSlot1(context: Context, uri: Uri) {
        val dir = memcardsDir(context).apply { mkdirs() }
        val outFile = File(dir, "Mcd001.ps2")
        try {
            context.contentResolver.openInputStream(uri)?.use { ins ->
                FileOutputStream(outFile).use { outs -> ins.copyTo(outs) }
            } ?: error("Could not open selected file")
            setDefaultSlots()
            status.value = "Imported Slot 1 as ${outFile.name}."
            Toast.makeText(context, "Memory card imported", Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            status.value = "Import failed: ${e.message ?: "unknown error"}"
            Toast.makeText(context, status.value, Toast.LENGTH_LONG).show()
        }
    }

    private fun importFolderCardSlot1(context: Context, uri: Uri) {
        val dir = memcardsDir(context).apply { mkdirs() }
        try {
            runCatching {
                context.contentResolver.takePersistableUriPermission(
                    uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION,
                )
            }

            val source = DocumentFile.fromTreeUri(context, uri)
                ?: error("Could not open selected folder")
            val dest = uniqueChild(dir, sanitizeFileName(source.name ?: "FolderCard"))
            dest.mkdirs()

            val copied = copyDocumentFolder(context, source, dest)
            if (copied == 0)
                error("Selected folder did not contain any files")

            if (assignSlot(context, 1, dest)) {
                status.value = "Imported folder card ${dest.name} to Slot 1."
                Toast.makeText(context, "Folder memory card imported", Toast.LENGTH_SHORT).show()
            }
        } catch (e: Exception) {
            status.value = "Folder import failed: ${e.message ?: "unknown error"}"
            Toast.makeText(context, status.value, Toast.LENGTH_LONG).show()
        }
    }

    private fun ensureDefaultSlots(context: Context) {
        memcardsDir(context).mkdirs()
        try {
            setDefaultSlots()
            status.value = "Default memory-card slots enabled."
            Toast.makeText(context, "Default memory-card slots enabled", Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            status.value = "Could not update memory-card settings: ${e.message ?: "unknown error"}"
            Toast.makeText(context, status.value, Toast.LENGTH_LONG).show()
        }
    }

    private fun assignSlot(context: Context, slot: Int, file: File): Boolean {
        if (!Main.nativeReady.value) {
            status.value = "Core settings are still starting up."
            Toast.makeText(context, status.value, Toast.LENGTH_LONG).show()
            return false
        }

        try {
            NativeApp.setSetting("MemoryCards", "Slot${slot}_Enable", "bool", "false")
            NativeApp.setSetting("MemoryCards", "Slot${slot}_Filename", "string", file.name)
            NativeApp.setSetting("MemoryCards", "Slot${slot}_Enable", "bool", "true")
            NativeApp.commitSettings()
            persistSlot(slot, file.name)
            status.value = "${file.name} assigned to Slot $slot."
            Toast.makeText(context, status.value, Toast.LENGTH_SHORT).show()
            return true
        } catch (e: Exception) {
            status.value = "Could not assign card: ${e.message ?: "unknown error"}"
            Toast.makeText(context, status.value, Toast.LENGTH_LONG).show()
            return false
        }
    }

    private fun setDefaultSlots() {
        if (!Main.nativeReady.value)
            error("core settings are still starting up")

        NativeApp.setSetting("MemoryCards", "Slot1_Enable", "bool", "false")
        NativeApp.setSetting("MemoryCards", "Slot1_Filename", "string", "Mcd001.ps2")
        NativeApp.setSetting("MemoryCards", "Slot1_Enable", "bool", "true")
        NativeApp.setSetting("MemoryCards", "Slot2_Enable", "bool", "false")
        NativeApp.setSetting("MemoryCards", "Slot2_Filename", "string", "Mcd002.ps2")
        NativeApp.setSetting("MemoryCards", "Slot2_Enable", "bool", "true")
        NativeApp.commitSettings()
        persistDefaultSlots()
    }

    private fun persistSlot(slot: Int, filename: String) {
        val current = ConfigStore.loadGlobal()
        val updated = when (slot) {
            1 -> current.copy(
                memoryCardSlot1Enabled = true,
                memoryCardSlot1Filename = filename,
            )
            2 -> current.copy(
                memoryCardSlot2Enabled = true,
                memoryCardSlot2Filename = filename,
            )
            else -> current
        }
        ConfigStore.saveGlobal(updated)
    }

    private fun persistDefaultSlots() {
        ConfigStore.saveGlobal(
            ConfigStore.loadGlobal().copy(
                memoryCardSlot1Enabled = true,
                memoryCardSlot1Filename = "Mcd001.ps2",
                memoryCardSlot2Enabled = true,
                memoryCardSlot2Filename = "Mcd002.ps2",
            )
        )
    }

    private fun memcardsDir(context: Context): File =
        File(Main.assetCopyRoot(context), "memcards")

    private fun copyDocumentFolder(context: Context, source: DocumentFile, dest: File): Int {
        var copied = 0
        for (child in source.listFiles()) {
            val name = sanitizeFileName(child.name ?: continue)
            if (child.isDirectory) {
                val childDest = File(dest, name).apply { mkdirs() }
                copied += copyDocumentFolder(context, child, childDest)
            } else if (child.isFile) {
                context.contentResolver.openInputStream(child.uri)?.use { input ->
                    FileOutputStream(File(dest, name)).use { output ->
                        input.copyTo(output)
                    }
                }
                copied++
            }
        }
        return copied
    }

    private fun uniqueChild(parent: File, requestedName: String): File {
        val base = requestedName.ifBlank { "FolderCard" }
        var candidate = File(parent, base)
        var suffix = 2
        while (candidate.exists()) {
            candidate = File(parent, "$base-$suffix")
            suffix++
        }
        return candidate
    }

    private fun sanitizeFileName(name: String): String =
        name.replace(Regex("""[\\/:*?"<>|]"""), "_").trim().ifBlank { "FolderCard" }

    @Composable
    private fun ps2ButtonColors() = ButtonDefaults.buttonColors(
        containerColor = Colors.pasx2_blue,
        contentColor = Color.White,
    )

    @Composable
    private fun darkButtonColors() = ButtonDefaults.buttonColors(
        containerColor = Color(0xFF2A2A2A),
        contentColor = Color.White,
    )

    private fun readableSize(bytes: Long): String {
        if (bytes <= 0L) return "-"
        val mib = bytes / (1024.0 * 1024.0)
        return if (mib >= 1.0) "%.1f MB".format(mib) else "${bytes / 1024L} KB"
    }
}
