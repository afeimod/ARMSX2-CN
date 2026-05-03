package com.armsx2.ui

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.Main
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kr.co.iefriends.pcsx2.NativeApp

/**
 * Save-state slot picker shared between Save and Load modes.
 *
 * 10 slots — matches PCSX2 convention. Each tile previews the slot via
 * `NativeApp.getImageSlot(slot)` (PNG-encoded snapshot) decoded to an
 * ImageBitmap, plus the originating game path from `getGamePathSlot(slot)`.
 * Empty slots show a placeholder; in Load mode they're disabled.
 *
 * On select:
 *   - Save: NativeApp.saveStateToSlot(slot), then onDone (closes overlay,
 *     resumes VM).
 *   - Load: NativeApp.loadStateFromSlot(slot), then onDone (same flow).
 *
 * Slot probe runs off-thread via produceState — getImageSlot is a JNI call
 * that touches disk for the snapshot blob.
 */
object SaveStatePicker {
    enum class Mode { Save, Load }

    private const val SLOTS = 10

    @Composable
    fun Render(mode: Mode, onDone: () -> Unit, onBack: () -> Unit) {
        // Save/load run on Dispatchers.IO — Main.invoke would have queued
        // the task behind the VM thread (single-threaded eDispatcher is
        // permanently blocked inside runVMThread's main loop), so the save
        // would never actually fire. IO pool is a separate thread, JNI
        // handles thread attachment fine. onDone hops back to Main for
        // the overlay state mutation.
        val scope = rememberCoroutineScope()
        Column(Modifier.fillMaxSize()) {
            Text(
                if (mode == Mode.Save) "Save State — pick a slot" else "Load State — pick a slot",
                color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.SemiBold,
            )
            Spacer(Modifier.height(8.dp))
            // 2 columns keeps the tiles big enough to read in the 420dp
            // overlay column. LazyVerticalGrid handles scroll if more
            // slots are added later.
            LazyVerticalGrid(
                columns = GridCells.Fixed(2),
                contentPadding = PaddingValues(2.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.weight(1f).fillMaxWidth(),
            ) {
                items((0 until SLOTS).toList(), key = { it }) { slot ->
                    SlotTile(slot, mode) { selected ->
                        when (mode) {
                            Mode.Save -> scope.launch(Dispatchers.IO) {
                                NativeApp.saveStateToSlot(selected)
                                withContext(Dispatchers.Main) { onDone() }
                            }
                            Mode.Load -> scope.launch(Dispatchers.IO) {
                                NativeApp.loadStateFromSlot(selected)
                                withContext(Dispatchers.Main) { onDone() }
                            }
                        }
                    }
                }
            }
            Spacer(Modifier.height(8.dp))
            BackRow(onBack)
        }
    }

    @Composable
    private fun SlotTile(slot: Int, mode: Mode, onPick: (Int) -> Unit) {
        // Probe the slot off-thread. Recompositions cheaply hit the cache.
        val gamePath by produceState<String?>(initialValue = null, slot) {
            value = withContext(Dispatchers.IO) { NativeApp.getGamePathSlot(slot) }
        }
        val image by produceState<android.graphics.Bitmap?>(initialValue = null, slot) {
            value = withContext(Dispatchers.IO) {
                runCatching {
                    val bytes = NativeApp.getImageSlot(slot) ?: return@runCatching null
                    if (bytes.isEmpty()) null else BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
                }.getOrNull()
            }
        }

        val empty = gamePath.isNullOrEmpty()
        // Load mode: empty slots are disabled. Save mode: tap = overwrite,
        // empty slots are valid targets.
        val enabled = mode == Mode.Save || !empty
        Column(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                .background(Color(0xFF272525).copy(alpha = 0.3f))
                .border(1.dp, Color(0xFF3A3A3A).copy(alpha = 0.6f), RoundedCornerShape(8.dp))
                .clickable(enabled = enabled, onClick = { onPick(slot) }),
        ) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .aspectRatio(4f / 3f)
                    .background(Color(0xFF1B1A1A).copy(alpha = 0.5f)),
                contentAlignment = Alignment.Center,
            ) {
                val bmp = image
                if (bmp != null) {
                    Image(
                        bitmap = bmp.asImageBitmap(),
                        contentDescription = "Slot $slot screenshot",
                        contentScale = ContentScale.Crop,
                        modifier = Modifier.fillMaxSize(),
                    )
                } else if (empty) {
                    Text("Empty", color = Color(0xFF6F6F6F), fontSize = 14.sp)
                } else {
                    // Path was populated but the snapshot bytes weren't —
                    // either still loading or the slot has no thumbnail.
                    Text("Slot $slot", color = Color(0xFF8F8F8F), fontSize = 12.sp)
                }
            }
            Column(Modifier.padding(8.dp)) {
                Text(
                    "Slot $slot",
                    color = Color.White,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                )
                if (!empty) {
                    Spacer(Modifier.height(2.dp))
                    Text(
                        gamePath?.substringAfterLast('/')?.substringBeforeLast('.') ?: "",
                        color = Color(0xFFAACCFF),
                        fontSize = 10.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                } else if (mode == Mode.Save) {
                    Spacer(Modifier.height(2.dp))
                    Text(
                        "(empty — tap to save here)",
                        color = Color(0xFF6F6F6F),
                        fontSize = 10.sp,
                    )
                }
            }
        }
    }

    @Composable
    private fun BackRow(onBack: () -> Unit) {
        Box(
            Modifier
                .fillMaxWidth()
                .height(40.dp)
                .clip(RoundedCornerShape(8.dp))
                .background(Color(0xFF272525).copy(alpha = 0.3f))
                .border(1.dp, Color(0xFF3A3A3A).copy(alpha = 0.6f), RoundedCornerShape(8.dp))
                .clickable(onClick = onBack)
                .padding(horizontal = 14.dp),
            contentAlignment = Alignment.CenterStart,
        ) {
            Text("Back", color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
        }
    }
}
