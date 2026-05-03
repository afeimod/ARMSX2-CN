package com.armsx2.ui.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.ui.Colors

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

/** Toggle row — label on left, status text on right. Tapping anywhere
 *  on the row flips the value via [onChange]. */
@Composable
fun ToggleRow(
    label: String,
    value: Boolean,
    onChange: (Boolean) -> Unit,
) {
    Box(
        Modifier
            .fillMaxWidth()
            .height(24.dp)
            .background(rowAura())
            .clickable { onChange(!value) }
            .padding(horizontal = 6.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
            Text(label, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            Spacer(Modifier.weight(1f))
            Text(
                if (value) "On" else "Off",
                color = if (value) Colors.pasx2_blue else Color(0xFF888888),
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
            )
        }
    }
}

/** Integer slider row — label + current value + slider below. The slider
 *  uses a tighter height than Material's default to keep the row at ~32dp
 *  so multiple slider rows still fit alongside toggle rows. */
@Composable
fun IntSliderRow(
    label: String,
    value: Int,
    min: Int,
    max: Int,
    valueFormatter: (Int) -> String = { it.toString() },
    onChange: (Int) -> Unit,
) {
    Box(
        Modifier
            .fillMaxWidth()
            .height(36.dp)
            .background(rowAura())
            .padding(horizontal = 6.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Column(modifier = Modifier.fillMaxWidth()) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(label, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
                Spacer(Modifier.weight(1f))
                Text(
                    valueFormatter(value),
                    color = Colors.pasx2_blue,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Bold,
                )
            }
            Slider(
                value = value.toFloat(),
                onValueChange = { onChange(it.toInt().coerceIn(min, max)) },
                valueRange = min.toFloat()..max.toFloat(),
                steps = (max - min - 1).coerceAtLeast(0),
                colors = SliderDefaults.colors(
                    thumbColor = Colors.pasx2_blue,
                    activeTrackColor = Colors.pasx2_blue,
                    inactiveTrackColor = Color(0xFF3A3A3A),
                ),
                modifier = Modifier.fillMaxWidth().height(16.dp),
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
