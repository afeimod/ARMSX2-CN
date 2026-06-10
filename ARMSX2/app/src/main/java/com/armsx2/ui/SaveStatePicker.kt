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
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyHorizontalGrid
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
import androidx.compose.ui.graphics.Brush
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
 * 10 numbered slots — matches PCSX2 convention. Each tile previews via
 * `NativeApp.getImageSlot(slot)` (PNG-encoded snapshot) plus the game
 * path from `getGamePathSlot(slot)`. Empty slots show a placeholder; in
 * Load mode they're disabled.
 *
 * Autosave tile (Load mode only, shown above slot 0 when present): the
 * "Save State And Exit" overlay action writes to a dedicated
 * `.autosave.p2s` file rather than slot 0, so numbered slots stay
 * user-controlled. The tile only appears when `hasAutosaveState` is
 * true on enter — probed off-thread at composition.
 *
 * On select:
 *   - Save: NativeApp.saveStateToSlot(slot), then onDone (closes overlay,
 *     resumes VM).
 *   - Load: NativeApp.loadStateFromSlot(slot) or loadAutosaveState() for
 *     the autosave tile, then onDone (same flow).
 *
 * Slot probe runs off-thread via produceState — getImageSlot is a JNI call
 * that touches disk for the snapshot blob.
 */
object SaveStatePicker {
    enum class Mode { Save, Load }

    private const val SLOTS = 10
    // Fixed tile width — LazyHorizontalGrid computes tile height from the
    // grid's intrinsic height ÷ rows. Width is up to the tile to set.
    private const val TILE_WIDTH_DP = 180

    @Composable
    fun Render(mode: Mode, onDone: () -> Unit, onBack: () -> Unit) {
        // Save/load run on Dispatchers.IO — Main.invoke would have queued
        // the task behind the VM thread (single-threaded eDispatcher is
        // permanently blocked inside runVMThread's main loop), so the save
        // would never actually fire. IO pool is a separate thread, JNI
        // handles thread attachment fine. onDone hops back to Main for
        // the overlay state mutation.
        val scope = rememberCoroutineScope()
        // Probe the autosave slot once at composition (Load mode only).
        // Off-thread because hasAutosaveState touches disk via
        // FileSystem::FileExists.
        val hasAutosave by produceState<Boolean>(initialValue = false, mode) {
            value = if (mode == Mode.Load) withContext(Dispatchers.IO) {
                NativeApp.hasAutosaveState()
            } else false
        }
        Column(Modifier.fillMaxSize()) {
            Text(
                if (mode == Mode.Save) "Save State" else "Load State",
                color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.SemiBold,
            )
            Spacer(Modifier.height(8.dp))
            // 2 columns keeps the tiles big enough to read in the 420dp
            // overlay column. LazyVerticalGrid handles scroll if more
            // slots are added later.
            // 2-row horizontal grid. Autosave (Load mode only) is the
            // leading tile, then numbered slots 0-9 flow column-by-column.
            // Scrolls horizontally — fits ~5-6 columns on a typical phone
            // landscape, rest accessible by swiping.
            LazyHorizontalGrid(
                rows = GridCells.Fixed(2),
                contentPadding = PaddingValues(2.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
                modifier = Modifier.weight(1f).fillMaxWidth(),
            ) {
                if (mode == Mode.Load && hasAutosave) {
                    item(key = "autosave") {
                        AutosaveTile {
                            scope.launch(Dispatchers.IO) {
                                NativeApp.loadAutosaveState()
                                withContext(Dispatchers.Main) { onDone() }
                            }
                        }
                    }
                }
                items((0 until SLOTS).toList(), key = { "slot_$it" }) { slot ->
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
    private fun AutosaveTile(onPick: () -> Unit) {
        val gamePath by produceState<String?>(initialValue = null) {
            value = withContext(Dispatchers.IO) { NativeApp.getAutosaveGamePath() }
        }
        val image by produceState<android.graphics.Bitmap?>(initialValue = null) {
            value = withContext(Dispatchers.IO) {
                runCatching {
                    val bytes = NativeApp.getAutosaveImage() ?: return@runCatching null
                    if (bytes.isEmpty()) null else BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
                }.getOrNull()
            }
        }
        TileFrame(
            borderColor = Color(0xFFFFB347).copy(alpha = 0.7f),
            backgroundColor = Color(0xFF2F2820).copy(alpha = 0.45f),
            onClick = onPick,
        ) {
            val bmp = image
            if (bmp != null) {
                Image(
                    bitmap = bmp.asImageBitmap(),
                    contentDescription = "Autosave screenshot",
                    contentScale = ContentScale.Crop,
                    modifier = Modifier.fillMaxSize(),
                )
            }
            BottomLabel(
                title = "Autosave",
                subtitle = gamePath?.substringAfterLast('/')?.substringBeforeLast('.')
                    ?: "(saved on exit)",
                titleColor = Color(0xFFFFB347),
            )
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
        TileFrame(
            borderColor = Color(0xFF3A3A3A).copy(alpha = 0.6f),
            backgroundColor = Color(0xFF272525).copy(alpha = 0.3f),
            enabled = enabled,
            onClick = { onPick(slot) },
        ) {
            val bmp = image
            if (bmp != null) {
                Image(
                    bitmap = bmp.asImageBitmap(),
                    contentDescription = "Slot $slot screenshot",
                    contentScale = ContentScale.Crop,
                    modifier = Modifier.fillMaxSize(),
                )
            }
            BottomLabel(
                title = "Slot $slot",
                subtitle = when {
                    !empty -> gamePath?.substringAfterLast('/')?.substringBeforeLast('.') ?: ""
                    mode == Mode.Save -> "(empty — tap to save here)"
                    else -> null
                },
                titleColor = Color.White,
            )
        }
    }

    // Tile chrome shared by AutosaveTile + SlotTile. Body content is rendered
    // edge-to-edge inside the rounded clip; BottomLabel can be dropped into
    // the same scope to overlay the title/subtitle row over the bottom of
    // the image.
    @Composable
    private fun TileFrame(
        borderColor: Color,
        backgroundColor: Color,
        enabled: Boolean = true,
        onClick: () -> Unit,
        content: @Composable androidx.compose.foundation.layout.BoxScope.() -> Unit,
    ) {
        Box(
            Modifier
                .width(TILE_WIDTH_DP.dp)
                .fillMaxHeight()
                .clip(RoundedCornerShape(8.dp))
                .background(backgroundColor)
                .border(1.dp, borderColor, RoundedCornerShape(8.dp))
                .clickable(enabled = enabled, onClick = onClick),
        ) {
            content()
        }
    }

    // Title + subtitle in the bottom-left of the tile, painted over a
    // left-to-right fade so the text stays readable against any thumbnail.
    // Designed to be invoked inside TileFrame so its Box parent provides
    // BoxScope alignment.
    @Composable
    private fun androidx.compose.foundation.layout.BoxScope.BottomLabel(
        title: String,
        subtitle: String?,
        titleColor: Color,
    ) {
        Box(
            Modifier
                .align(Alignment.BottomStart)
                .fillMaxWidth()
                .background(
                    Brush.horizontalGradient(
                        0.0f to Color.Black.copy(alpha = 0.75f),
                        0.6f to Color.Black.copy(alpha = 0.45f),
                        1.0f to Color.Transparent,
                    )
                )
                .padding(horizontal = 8.dp, vertical = 6.dp),
        ) {
            Column {
                Text(
                    title,
                    color = titleColor,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                )
                if (!subtitle.isNullOrEmpty()) {
                    Spacer(Modifier.height(2.dp))
                    Text(
                        subtitle,
                        color = Color(0xFFAACCFF),
                        fontSize = 10.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
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
