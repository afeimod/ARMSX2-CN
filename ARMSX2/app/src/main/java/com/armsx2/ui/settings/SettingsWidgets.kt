package com.armsx2.ui.settings

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.ui.Colors
import kotlin.math.roundToInt

/**
 * Shared widget primitives for the in-game settings tabs. Style matches
 * InGameOverlay's MenuRow — left-anchored alpha aura on the row bg,
 * white text, transparent borders. Each row is 24dp tall to keep the
 * tab content fitting in the same 75% screen height the Playing-Now
 * tab uses.
 */

private val rowAuraOn = Color.White.copy(alpha = 0.06f)
private val rowAuraTransparent = Color.Transparent

/** Horizontal background gradient that matches the divider direction. */
internal fun rowAura() = Brush.horizontalGradient(listOf(rowAuraOn, rowAuraTransparent))

/** Thin horizontal divider with left-anchored fade. Mirrors InGameOverlay's
 *  MenuDivider so settings rows tie visually into the existing overlay. */
@Composable
fun SettingsDivider() {
    Box(
        Modifier
            .fillMaxWidth()
            .height(1.dp)
            .background(
                Brush.horizontalGradient(
                    listOf(Color.White.copy(alpha = 0.35f), Color.Transparent)
                )
            ),
    )
}

@Composable
fun HelpText(text: String, modifier: Modifier = Modifier) {
    Text(
        text = text,
        color = Color(0xFFB8B8B8),
        fontSize = 10.sp,
        lineHeight = 12.sp,
        modifier = modifier.padding(horizontal = 6.dp, vertical = 4.dp),
    )
}

/** Toggle row — label on left, status text on right. Tapping anywhere
 *  on the row flips the value via [onChange]. */
@Composable
fun ToggleRow(
    label: String,
    value: Boolean,
    description: String? = null,
    onChange: (Boolean) -> Unit,
) {
    Box(
        Modifier
            .fillMaxWidth()
            .height(if (description == null) 24.dp else 42.dp)
            .background(rowAura())
            .clickable { onChange(!value) }
            .padding(horizontal = 6.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Column(modifier = Modifier.weight(1f)) {
                Text(label, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                if (description != null) {
                    Text(
                        description,
                        color = Color(0xFFB8B8B8),
                        fontSize = 9.sp,
                        lineHeight = 10.sp,
                        maxLines = 2,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
            Text(
                if (value) "On" else "Off",
                color = if (value) Colors.pasx2_blue else Color(0xFF888888),
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
            )
        }
    }
}

/** Compact On/Off bubble — same visual language as the Playing-Now grid
 *  in [InGameOverlay]. Label on top (1–2 lines), state line ("On"/"Off")
 *  below. When the toggle is active the surface uses the PS2-blue accent
 *  treatment; inactive matches the neutral bubble surface. Caller is
 *  responsible for laying these out in a [BubbleGridRow]. */
@Composable
fun ToggleBubble(
    label: String,
    value: Boolean,
    modifier: Modifier = Modifier,
    onChange: (Boolean) -> Unit,
) {
    val bg: Color
    val border: Color
    if (value) {
        bg = Color(0xFF222F40)
        border = Colors.pasx2_blue.copy(alpha = 0.50f)
    } else {
        bg = Color(0xFF1F2123)
        border = Color.White.copy(alpha = 0.10f)
    }
    Column(
        modifier = modifier
            .aspectRatio(1.35f)
            .clip(RoundedCornerShape(10.dp))
            .background(bg)
            .border(1.dp, border, RoundedCornerShape(10.dp))
            .clickable { onChange(!value) }
            .padding(horizontal = 4.dp, vertical = 4.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(
            label,
            color = Color.White,
            fontSize = 9.sp,
            fontWeight = FontWeight.Bold,
            textAlign = TextAlign.Center,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
        )
        Text(
            if (value) "On" else "Off",
            color = if (value) Colors.pasx2_blue else Color.White.copy(alpha = 0.6f),
            fontSize = 10.sp,
            fontWeight = FontWeight.Bold,
            textAlign = TextAlign.Center,
            maxLines = 1,
        )
    }
}

/** Even-spaced four-cell row for [ToggleBubble] grids. Mirrors the
 *  Playing-Now layout so the two surfaces feel like the same component
 *  family. Use [Modifier.weight] inside `content` on each child. */
@Composable
fun BubbleGridRow(content: @Composable RowScope.() -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        content = content,
    )
}

/** Integer slider row — label + current value on top line, custom slim
 *  slider below. Replaces Material3's chunky default with a Canvas-drawn
 *  slider that matches the overlay's thin-line aesthetic: 3dp track,
 *  tick dots at each discrete step, 5dp thumb with a soft halo. Drag
 *  and tap-to-position both update the value. */
@Composable
fun IntSliderRow(
    label: String,
    value: Int,
    min: Int,
    max: Int,
    description: String? = null,
    valueFormatter: (Int) -> String = { it.toString() },
    onChange: (Int) -> Unit,
) {
    Box(
        Modifier
            .fillMaxWidth()
            .height(if (description == null) 36.dp else 50.dp)
            .background(rowAura())
            .padding(horizontal = 6.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Column(modifier = Modifier.fillMaxWidth()) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(label, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                    if (description != null) {
                        Text(
                            description,
                            color = Color(0xFFB8B8B8),
                            fontSize = 9.sp,
                            lineHeight = 10.sp,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    }
                }
                Text(
                    valueFormatter(value),
                    color = Colors.pasx2_blue,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                )
            }
            Spacer(Modifier.height(3.dp))
            DiscreteSlider(
                value = value,
                min = min,
                max = max,
                onChange = onChange,
            )
        }
    }
}

/** Custom slim discrete slider. Canvas-drawn so we don't have to fight
 *  Material's default touch target / thumb sizing. The thumb sits at the
 *  current step, tick dots mark every step in the range, and the active
 *  track fills from min to the current value in PS2 blue. */
@Composable
private fun DiscreteSlider(
    value: Int,
    min: Int,
    max: Int,
    onChange: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    val steps = (max - min).coerceAtLeast(1)
    val frac = ((value - min).toFloat() / steps.toFloat()).coerceIn(0f, 1f)
    val activeColor = Colors.pasx2_blue
    val inactiveTrackColor = Color.White.copy(alpha = 0.12f)
    val inactiveTickColor = Color.White.copy(alpha = 0.35f)

    Box(
        modifier = modifier
            .fillMaxWidth()
            .height(14.dp)
            .pointerInput(min, max) {
                val edgePx = 6.dp.toPx()
                fun update(x: Float) {
                    val usable = (size.width - edgePx * 2).coerceAtLeast(1f)
                    val f = ((x - edgePx) / usable).coerceIn(0f, 1f)
                    onChange(min + (f * steps).roundToInt())
                }
                detectTapGestures { update(it.x) }
            }
            .pointerInput(min, max) {
                val edgePx = 6.dp.toPx()
                detectHorizontalDragGestures { change, _ ->
                    val usable = (size.width - edgePx * 2).coerceAtLeast(1f)
                    val f = ((change.position.x - edgePx) / usable).coerceIn(0f, 1f)
                    onChange(min + (f * steps).roundToInt())
                }
            },
    ) {
        Canvas(modifier = Modifier.fillMaxSize()) {
            val edgePx = 6.dp.toPx()
            val usable = (size.width - edgePx * 2).coerceAtLeast(1f)
            val centerY = size.height / 2f
            val trackThickness = 3.dp.toPx()
            val trackY = centerY - trackThickness / 2f

            // Inactive track (full width between edge insets).
            drawRoundRect(
                color = inactiveTrackColor,
                topLeft = Offset(edgePx, trackY),
                size = Size(usable, trackThickness),
                cornerRadius = CornerRadius(trackThickness / 2f),
            )
            // Active fill (min → current value).
            drawRoundRect(
                color = activeColor,
                topLeft = Offset(edgePx, trackY),
                size = Size(usable * frac, trackThickness),
                cornerRadius = CornerRadius(trackThickness / 2f),
            )

            // Tick dots — only when the step count is small enough to read.
            // Cycle Rate is -3..3 (7 steps), Cycle Skip is 0..3 (4 steps),
            // both well within range. For wider sliders the ticks would
            // crowd, so we skip them.
            if (steps in 2..12) {
                val tickRadius = 1.5.dp.toPx()
                for (i in 0..steps) {
                    val tickFrac = i.toFloat() / steps
                    val tickX = edgePx + usable * tickFrac
                    val onActive = tickFrac <= frac
                    drawCircle(
                        color = if (onActive) activeColor else inactiveTickColor,
                        radius = tickRadius,
                        center = Offset(tickX, centerY),
                    )
                }
            }

            // Thumb: outer halo + solid body + inner highlight pip. The
            // halo softens the dot against the dim backdrop without making
            // the thumb feel as heavy as Material's default 20dp circle.
            val thumbX = edgePx + usable * frac
            drawCircle(
                color = activeColor.copy(alpha = 0.25f),
                radius = 7.dp.toPx(),
                center = Offset(thumbX, centerY),
            )
            drawCircle(
                color = activeColor,
                radius = 5.dp.toPx(),
                center = Offset(thumbX, centerY),
            )
            drawCircle(
                color = Color.White,
                radius = 1.5.dp.toPx(),
                center = Offset(thumbX, centerY),
            )
        }
    }
}

/** Segmented chooser row — label on left, horizontal chip strip on right.
 *  Each option is a tappable chip; selected chip highlights in PS2 blue.
 *  Suitable for short option lists (3–6) — for longer lists prefer a
 *  dropdown widget (TODO when needed). */
@Composable
fun SegmentedRow(
    label: String,
    options: List<String>,
    selectedIndex: Int,
    description: String? = null,
    onChange: (Int) -> Unit,
) {
    Box(
        Modifier
            .fillMaxWidth()
            .background(rowAura())
            .padding(horizontal = 6.dp, vertical = 3.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Column {
            Text(label, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            if (description != null) {
                Spacer(Modifier.height(1.dp))
                Text(
                    description,
                    color = Color(0xFFB8B8B8),
                    fontSize = 9.sp,
                    lineHeight = 10.sp,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Spacer(Modifier.height(3.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                options.forEachIndexed { idx, option ->
                    val on = idx == selectedIndex
                    Box(
                        Modifier
                            .clip(RoundedCornerShape(4.dp))
                            .background(if (on) Colors.pasx2_blue else Color(0xFF272525).copy(alpha = 0.5f))
                            .clickable { onChange(idx) }
                            .padding(horizontal = 8.dp, vertical = 3.dp),
                    ) {
                        Text(
                            option,
                            color = if (on) Color.White else Color(0xFFAAAAAA),
                            fontSize = 11.sp,
                            fontWeight = FontWeight.Bold,
                        )
                    }
                }
            }
        }
    }
}

/** Multi-row variant for longer option lists. Keeps deinterlacing / hardware
 *  download choices readable inside the compact in-game overlay instead of
 *  letting a long chip strip run off-screen. */
@Composable
fun SegmentedGridRow(
    label: String,
    options: List<String>,
    selectedIndex: Int,
    columns: Int = 3,
    description: String? = null,
    onChange: (Int) -> Unit,
) {
    Box(
        Modifier
            .fillMaxWidth()
            .background(rowAura())
            .padding(horizontal = 6.dp, vertical = 3.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Column {
            Text(label, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            if (description != null) {
                Spacer(Modifier.height(1.dp))
                Text(
                    description,
                    color = Color(0xFFB8B8B8),
                    fontSize = 9.sp,
                    lineHeight = 10.sp,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Spacer(Modifier.height(3.dp))
            Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                options.chunked(columns.coerceAtLeast(1)).forEachIndexed { rowIndex, rowOptions ->
                    Row(horizontalArrangement = Arrangement.spacedBy(4.dp), modifier = Modifier.fillMaxWidth()) {
                        rowOptions.forEachIndexed { colIndex, option ->
                            val idx = rowIndex * columns.coerceAtLeast(1) + colIndex
                            val on = idx == selectedIndex
                            Box(
                                Modifier
                                    .weight(1f)
                                    .clip(RoundedCornerShape(4.dp))
                                    .background(if (on) Colors.pasx2_blue else Color(0xFF272525).copy(alpha = 0.5f))
                                    .clickable { onChange(idx) }
                                    .padding(horizontal = 5.dp, vertical = 3.dp),
                                contentAlignment = Alignment.Center,
                            ) {
                                Text(
                                    option,
                                    color = if (on) Color.White else Color(0xFFAAAAAA),
                                    fontSize = 10.sp,
                                    fontWeight = FontWeight.Bold,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                )
                            }
                        }
                        repeat(columns.coerceAtLeast(1) - rowOptions.size) {
                            Spacer(Modifier.weight(1f))
                        }
                    }
                }
            }
        }
    }
}
