package com.armsx2.ui.touch

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.PointerInputChange
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.armsx2.EmuState
import com.armsx2.Main
import com.armsx2.R
import com.armsx2.ui.Colors
import com.armsx2.ui.WindowImpl
import kr.co.iefriends.pcsx2.NativeApp
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.min

/** Root entry point. Place this in the same fillMaxSize() container as
 *  the AndroidView surface. Renders nothing when the VM isn't running
 *  or controls are hidden by the controller-mode latch (unless in edit
 *  mode, which forces the buttons visible). */
@Composable
fun TouchControlsOverlay() {
    TouchControls.ensureLoaded()
    val running = Main.eState.value == EmuState.RUNNING ||
                  Main.eState.value == EmuState.PAUSED
    if (!running) return
    val edit = TouchControls.editMode.value
    // Hide while the pause overlay is up so the pause menu owns the screen.
    // In edit mode we ignore overlayVisible — the user enters edit mode
    // from the pause menu, and the overlay closes itself when toggling on.
    if (WindowImpl.overlayVisible.value && !edit) return
    // Same for the game library overlay (Load Game button while a game
    // is running) — it sits on top of the surface, the touch buttons
    // shouldn't paint over the library cards.
    if (WindowImpl.showLibrary.value && !edit) return
    if (!TouchControls.visible.value && !edit) return

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val w = maxWidth
        val h = maxHeight
        val density = LocalDensity.current
        val widthPx = with(density) { w.toPx() }
        val heightPx = with(density) { h.toPx() }
        LaunchedEffect(widthPx, heightPx) {
            OverlayDims.last = OverlayDims.Dims(widthPx, heightPx)
        }
        val layout = TouchControls.activeLayout.value

        if (edit) {
            // Dim backdrop. Two jobs:
            //   1. Consume every pointer change so long-press on empty
            //      space can't leak to the AndroidView and re-open the
            //      pause menu mid-edit.
            //   2. Clear the current widget selection if the user taps
            //      empty space (gesture is a tap when the pointer comes
            //      up without significant motion).
            Box(
                Modifier
                    .fillMaxSize()
                    .background(Color(0xFF000000).copy(alpha = 0.35f))
                    .pointerInput(Unit) {
                        awaitPointerEventScope {
                            while (true) {
                                val downEv = awaitPointerEvent()
                                val firstChange = downEv.changes.firstOrNull { it.pressed }
                                downEv.changes.forEach { it.consume() }
                                if (firstChange == null) continue
                                val downPos = firstChange.position
                                var moved = false
                                // Track until the pointer comes up.
                                while (true) {
                                    val ev = awaitPointerEvent()
                                    ev.changes.forEach { it.consume() }
                                    val c = ev.changes.firstOrNull { it.id == firstChange.id }
                                        ?: break
                                    val dx = c.position.x - downPos.x
                                    val dy = c.position.y - downPos.y
                                    if (dx * dx + dy * dy > 64f) moved = true
                                    if (!c.pressed) break
                                }
                                if (!moved) {
                                    TouchControls.selectedButton.value = null
                                }
                            }
                        }
                    },
            )
        }

        for (cfg in layout.buttons) {
            if (!cfg.enabled && !edit) continue
            val size = cfg.sizeDp.dp
            val cx = w * cfg.xFrac
            val cy = h * cfg.yFrac
            val left = cx - size / 2
            val top = cy - size / 2
            Box(
                modifier = Modifier
                    .offset(x = left, y = top)
                    .size(size),
            ) {
                when (cfg.id.kind) {
                    TouchButtonId.Kind.DPAD -> DpadWidget(cfg, edit)
                    TouchButtonId.Kind.STICK -> StickWidget(cfg, edit)
                    TouchButtonId.Kind.PAUSE -> PauseWidget(cfg, edit)
                    else -> ButtonWidget(cfg, edit)
                }
            }
        }

        if (edit) {
            EditToolbar(
                modifier = Modifier
                    .align(Alignment.TopCenter)
                    .padding(top = 12.dp),
            )
        }

        if (TouchControls.profileDialogOpen.value) {
            ProfilePicker(onDismiss = { TouchControls.profileDialogOpen.value = false })
        }
    }
}

/* -------------------------------------------------------------------- */
/*  Digital button widget                                                */
/* -------------------------------------------------------------------- */

@Composable
private fun ButtonWidget(cfg: TouchButtonCfg, edit: Boolean) {
    var pressed by remember(cfg.id) { mutableStateOf(false) }
    val opacity = TouchControls.opacity.value
    val mod = Modifier
        .fillMaxSize()
        .let {
            if (edit) it.editGestures(cfg)
            else it.pressGestures(cfg.id.keycode) { p -> pressed = p }
        }
    // Pressed feedback: every button shrinks a hair AND darkens.
    // - Buttons with a pressed sprite (most) get the darken via the
    //   already-darker PNG swap.
    // - CIRCLE / SQUARE don't ship pressed sprites, so they get the
    //   darken via ColorFilter.tint + BlendMode.Modulate (multiplies
    //   each pixel by the mid-gray tint).
    // Both groups share the same scale-down so the shrink-on-press
    // feel is consistent.
    val darkenOnPress = pressed && !hasPressedSprite(cfg.id)
    Box(modifier = mod, contentAlignment = Alignment.Center) {
        Image(
            painter = painterResource(drawableFor(cfg.id, pressed)),
            contentDescription = cfg.id.label,
            contentScale = ContentScale.Fit,
            alpha = opacity,
            colorFilter = if (darkenOnPress)
                ColorFilter.tint(androidx.compose.ui.graphics.Color(0xFFB0B0B0), BlendMode.Modulate)
            else null,
            modifier = Modifier
                .fillMaxSize()
                // CIRCLE / SQUARE shrink more (0.85) because they lack
                // a pressed PNG — the rest get a smaller 0.92 nudge on
                // top of the pressed-sprite swap, which itself depicts
                // a slightly inset button.
                .scale(
                    when {
                        !pressed -> 1f
                        hasPressedSprite(cfg.id) -> 0.92f
                        else -> 0.85f
                    }
                ),
        )
        if (edit) EditAdornment(cfg.id)
    }
}

/** True if the asset ships a separate pressed variant. CIRCLE and
 *  SQUARE only have the resting sprite. */
private fun hasPressedSprite(id: TouchButtonId): Boolean = when (id) {
    TouchButtonId.CIRCLE, TouchButtonId.SQUARE -> false
    else -> true
}

/** Map a button id + press state to the bundled PNG. CIRCLE / SQUARE
 *  ship without a separate pressed sprite, so they reuse their default
 *  for both states. */
private fun drawableFor(id: TouchButtonId, pressed: Boolean): Int = when (id) {
    TouchButtonId.CROSS    -> if (pressed) R.drawable.pad_cross_pressed    else R.drawable.pad_cross
    TouchButtonId.CIRCLE   -> R.drawable.pad_circle
    TouchButtonId.SQUARE   -> R.drawable.pad_square
    TouchButtonId.TRIANGLE -> if (pressed) R.drawable.pad_triangle_pressed else R.drawable.pad_triangle
    TouchButtonId.L1       -> if (pressed) R.drawable.pad_l1_pressed       else R.drawable.pad_l1
    TouchButtonId.L2       -> if (pressed) R.drawable.pad_l2_pressed       else R.drawable.pad_l2
    TouchButtonId.R1       -> if (pressed) R.drawable.pad_r1_pressed       else R.drawable.pad_r1
    TouchButtonId.R2       -> if (pressed) R.drawable.pad_r2_pressed       else R.drawable.pad_r2
    TouchButtonId.START    -> if (pressed) R.drawable.pad_start_pressed    else R.drawable.pad_start
    TouchButtonId.SELECT   -> if (pressed) R.drawable.pad_select_pressed   else R.drawable.pad_select
    TouchButtonId.L3       -> if (pressed) R.drawable.pad_l3_pressed       else R.drawable.pad_l3
    TouchButtonId.R3       -> if (pressed) R.drawable.pad_r3_pressed       else R.drawable.pad_r3
    // DPad / sticks render their own composed sprites; PAUSE renders none.
    TouchButtonId.DPAD, TouchButtonId.L_STICK, TouchButtonId.R_STICK,
    TouchButtonId.PAUSE -> R.drawable.pad_cross
}

/* -------------------------------------------------------------------- */
/*  Pause hotspot — invisible long-press zone that opens the overlay    */
/* -------------------------------------------------------------------- */

/** Invisible in play mode; long-press opens the in-game pause overlay.
 *  Replaced the old long-press-anywhere surface gesture (see Main.kt),
 *  which paused on accidental presses in empty screen space. The default
 *  spot sits between the DPad and the face-button diamond; in edit mode
 *  it renders an outlined "PAUSE" box so it can be dragged/resized like
 *  any other widget. */
@Composable
private fun PauseWidget(cfg: TouchButtonCfg, edit: Boolean) {
    if (edit) {
        Box(
            modifier = Modifier.fillMaxSize().editGestures(cfg),
            contentAlignment = Alignment.Center,
        ) {
            EditAdornment(cfg.id)
            Text(
                "PAUSE",
                color = Color.White.copy(alpha = 0.75f),
                fontSize = 12.sp,
                fontWeight = FontWeight.SemiBold,
            )
        }
    } else {
        Box(
            modifier = Modifier
                .fillMaxSize()
                .pointerInput(cfg.id) {
                    detectTapGestures(
                        onLongPress = { com.armsx2.ui.InGameOverlay.open() },
                    )
                },
        )
    }
}

/* -------------------------------------------------------------------- */
/*  DPad widget — single 4-way pad emitting up/down/left/right          */
/* -------------------------------------------------------------------- */

@Composable
private fun DpadWidget(cfg: TouchButtonCfg, edit: Boolean) {
    val active = remember(cfg.id) { mutableStateOf(DpadState()) }
    val opacity = TouchControls.opacity.value

    val pressMod: Modifier = if (edit) {
        Modifier.editGestures(cfg)
    } else {
        Modifier.pointerInput(cfg.id) {
            awaitPointerEventScope {
                while (true) {
                    val ev = awaitPointerEvent()
                    val change = ev.changes.firstOrNull() ?: continue
                    if (!change.pressed) {
                        if (active.value.any()) {
                            releaseDpad(active.value)
                            active.value = DpadState()
                        }
                        continue
                    }
                    val pos = change.position
                    val cx = size.width / 2f
                    val cy = size.height / 2f
                    val dx = pos.x - cx
                    val dy = pos.y - cy
                    val deadR = min(cx, cy) * 0.22f
                    val r = hypot(dx, dy)
                    // 8-way with cardinal-biased sectors: the minor axis
                    // only fires when its magnitude is at least
                    // `diagBias` of the major axis. With diagBias=0.55
                    // that's an angle within ~29° of 45° — a ~58° wedge
                    // around each diagonal; everything outside snaps to
                    // the dominant cardinal so a slightly-angled press
                    // doesn't fire two axes by accident.
                    val target = if (r < deadR) DpadState() else {
                        val absDx = kotlin.math.abs(dx)
                        val absDy = kotlin.math.abs(dy)
                        val diagBias = 0.55f
                        val keepX = absDx >= absDy * diagBias
                        val keepY = absDy >= absDx * diagBias
                        DpadState(
                            up    = keepY && dy < 0f,
                            down  = keepY && dy > 0f,
                            left  = keepX && dx < 0f,
                            right = keepX && dx > 0f,
                        )
                    }
                    if (target != active.value) {
                        applyDpadDiff(active.value, target)
                        active.value = target
                    }
                }
            }
        }
    }

    // Up / Left / Right ship as bundled sprites — each is the arm of
    // the DPad pointing inward to the center. Down reuses the up sprite
    // rotated 180° (so the asset author only had to ship 3 arms).
    // Each arm fills ~half the DPad's width or height and aligns to its
    // edge so the four arms compose into a + shape.
    // Aspect ratios from the tight-cropped sprite dimensions (see
    // crop_labels.py). All sprites are now tight-cropped so the
    // arm-to-canvas ratio is consistent across U/D/L/R — without this
    // the un-labelled up sprite rendered ~half size next to its
    // label-stripped L/R siblings.
    val upRatio    = 45f / 52f
    val lrRatio    = 53f / 44f
    Box(
        modifier = Modifier.fillMaxSize().then(pressMod),
    ) {
        // Only the UP arm needs a nudge — the tight crop trimmed some
        // AA off the outer flat edge so without this it reads "pushed
        // inwards" toward the center. The down arm uses the same
        // sprite rotated 180° and sat correctly already.
        Image(
            painter = painterResource(
                if (active.value.up) R.drawable.pad_dpad_up_pressed else R.drawable.pad_dpad_up
            ),
            contentDescription = "DPad up",
            contentScale = ContentScale.Fit,
            alpha = opacity,
            modifier = Modifier
                .align(Alignment.TopCenter)
                .offset(y = (-4).dp)
                .fillMaxHeight(0.5f)
                .aspectRatio(upRatio),
        )
        Image(
            painter = painterResource(
                if (active.value.down) R.drawable.pad_dpad_up_pressed else R.drawable.pad_dpad_up
            ),
            contentDescription = "DPad down",
            contentScale = ContentScale.Fit,
            alpha = opacity,
            modifier = Modifier
                .align(Alignment.BottomCenter)
                .fillMaxHeight(0.5f)
                .aspectRatio(upRatio)
                .rotate(180f),
        )
        Image(
            painter = painterResource(
                if (active.value.left) R.drawable.pad_dpad_left_pressed else R.drawable.pad_dpad_left
            ),
            contentDescription = "DPad left",
            contentScale = ContentScale.Fit,
            alpha = opacity,
            modifier = Modifier
                .align(Alignment.CenterStart)
                .fillMaxWidth(0.5f)
                .aspectRatio(lrRatio),
        )
        Image(
            painter = painterResource(
                if (active.value.right) R.drawable.pad_dpad_right_pressed else R.drawable.pad_dpad_right
            ),
            contentDescription = "DPad right",
            contentScale = ContentScale.Fit,
            alpha = opacity,
            modifier = Modifier
                .align(Alignment.CenterEnd)
                .fillMaxWidth(0.5f)
                .aspectRatio(lrRatio),
        )
        if (edit) EditAdornment(cfg.id)
    }
}

private data class DpadState(
    val up: Boolean = false,
    val down: Boolean = false,
    val left: Boolean = false,
    val right: Boolean = false,
) {
    fun any() = up || down || left || right
}

private fun applyDpadDiff(prev: DpadState, next: DpadState) {
    if (prev.up    != next.up)    sendDigital(19, next.up)
    if (prev.down  != next.down)  sendDigital(20, next.down)
    if (prev.left  != next.left)  sendDigital(21, next.left)
    if (prev.right != next.right) sendDigital(22, next.right)
}

private fun releaseDpad(state: DpadState) {
    if (state.up)    sendDigital(19, false)
    if (state.down)  sendDigital(20, false)
    if (state.left)  sendDigital(21, false)
    if (state.right) sendDigital(22, false)
}

/* -------------------------------------------------------------------- */
/*  Analog stick widget                                                  */
/* -------------------------------------------------------------------- */

@Composable
private fun StickWidget(cfg: TouchButtonCfg, edit: Boolean) {
    val thumb = remember(cfg.id) { mutableStateOf(Offset.Zero) }
    val lastEmit = remember(cfg.id) { mutableStateOf(StickEmit()) }
    val opacity = TouchControls.opacity.value
    val density = LocalDensity.current

    // See Main.dispatchGenericMotionEvent for the L / R axis mappings —
    // posCode / negCode per axis.
    val codes = when (cfg.id) {
        TouchButtonId.L_STICK -> StickCodes(xPos = 111, xNeg = 113, yPos = 112, yNeg = 110)
        else -> StickCodes(xPos = 121, xNeg = 123, yPos = 122, yNeg = 120)
    }

    val pressMod: Modifier = if (edit) {
        Modifier.editGestures(cfg)
    } else {
        Modifier.pointerInput(cfg.id) {
            val radiusPx = with(density) { (cfg.sizeDp / 2f).dp.toPx() }
            awaitPointerEventScope {
                while (true) {
                    val ev = awaitPointerEvent()
                    val change = ev.changes.firstOrNull() ?: continue
                    if (!change.pressed) {
                        thumb.value = Offset.Zero
                        if (lastEmit.value.any()) {
                            releaseStick(codes, lastEmit.value)
                            lastEmit.value = StickEmit()
                        }
                        continue
                    }
                    val cxLocal = size.width / 2f
                    val cyLocal = size.height / 2f
                    val dx = change.position.x - cxLocal
                    val dy = change.position.y - cyLocal
                    val r = hypot(dx, dy)
                    // Visual thumb caps inside the ring; force is
                    // normalized against the same cap.
                    val capPx = radiusPx * 0.66f
                    val scale = if (r > capPx) capPx / r else 1f
                    val capDx = dx * scale
                    val capDy = dy * scale
                    thumb.value = Offset(capDx, capDy)
                    val nx = (capDx / capPx).coerceIn(-1f, 1f)
                    val ny = (capDy / capPx).coerceIn(-1f, 1f)
                    val emit = computeStickEmit(nx, ny)
                    if (emit != lastEmit.value) {
                        applyStickDiff(codes, lastEmit.value, emit)
                        lastEmit.value = emit
                    }
                }
            }
        }
    }

    // Textured base + thumb. The base PNG paints the static ring; the
    // thumb is `pad_thumb` — a textured circle moved by the user's
    // finger. The L3 / R3 stick-CLICK action is a separate button id
    // (see TouchButtonId.L3 / R3), not the stick widget itself, so the
    // thumb sprite has no pressed variant.
    // Outer Box is NOT clipped, so when the finger is past the cap the
    // thumb can render OVER the ring.
    Box(
        modifier = Modifier.fillMaxSize().then(pressMod),
    ) {
        Image(
            painter = painterResource(R.drawable.pad_stick_base),
            contentDescription = cfg.id.label + " base",
            contentScale = ContentScale.Fit,
            alpha = opacity,
            modifier = Modifier.fillMaxSize(),
        )
        val thumbSizeDp = cfg.sizeDp * 0.62f
        Image(
            painter = painterResource(R.drawable.pad_thumb),
            contentDescription = cfg.id.label + " thumb",
            contentScale = ContentScale.Fit,
            alpha = opacity,
            modifier = Modifier
                .align(Alignment.Center)
                .offset(
                    x = with(density) { thumb.value.x.toDp() },
                    y = with(density) { thumb.value.y.toDp() },
                )
                .size(thumbSizeDp.dp),
        )
        if (edit) EditAdornment(cfg.id)
    }
}

private data class StickCodes(val xPos: Int, val xNeg: Int, val yPos: Int, val yNeg: Int)

private data class StickEmit(
    val xPos: Int = 0,
    val xNeg: Int = 0,
    val yPos: Int = 0,
    val yNeg: Int = 0,
) {
    fun any() = xPos != 0 || xNeg != 0 || yPos != 0 || yNeg != 0
}

private const val STICK_DEAD = 0.10f

private fun computeStickEmit(nx: Float, ny: Float): StickEmit {
    val absX = abs(nx)
    val absY = abs(ny)
    val scaleX = if (absX > STICK_DEAD) (absX * 32767f).toInt() else 0
    val scaleY = if (absY > STICK_DEAD) (absY * 32767f).toInt() else 0
    return StickEmit(
        xPos = if (nx > 0) scaleX else 0,
        xNeg = if (nx < 0) scaleX else 0,
        yPos = if (ny > 0) scaleY else 0,
        yNeg = if (ny < 0) scaleY else 0,
    )
}

private fun applyStickDiff(codes: StickCodes, prev: StickEmit, next: StickEmit) {
    if (prev.xPos != next.xPos) NativeApp.setPadButton(codes.xPos, next.xPos, next.xPos > 0)
    if (prev.xNeg != next.xNeg) NativeApp.setPadButton(codes.xNeg, next.xNeg, next.xNeg > 0)
    if (prev.yPos != next.yPos) NativeApp.setPadButton(codes.yPos, next.yPos, next.yPos > 0)
    if (prev.yNeg != next.yNeg) NativeApp.setPadButton(codes.yNeg, next.yNeg, next.yNeg > 0)
}

private fun releaseStick(codes: StickCodes, last: StickEmit) {
    if (last.xPos != 0) NativeApp.setPadButton(codes.xPos, 0, false)
    if (last.xNeg != 0) NativeApp.setPadButton(codes.xNeg, 0, false)
    if (last.yPos != 0) NativeApp.setPadButton(codes.yPos, 0, false)
    if (last.yNeg != 0) NativeApp.setPadButton(codes.yNeg, 0, false)
}

/* -------------------------------------------------------------------- */
/*  Gesture helpers                                                      */
/* -------------------------------------------------------------------- */

private fun sendDigital(keycode: Int, pressed: Boolean) {
    NativeApp.setPadButton(keycode, 0, pressed)
}

/** Press/release pointerInput for a single digital button. Emits the
 *  keycode on down, releases on up or pointer cancel. */
private fun Modifier.pressGestures(keycode: Int, onPressedChange: (Boolean) -> Unit) =
    pointerInput(keycode) {
        awaitPointerEventScope {
            while (true) {
                val ev = awaitPointerEvent()
                val change: PointerInputChange = ev.changes.firstOrNull() ?: continue
                if (!change.pressed) continue
                onPressedChange(true)
                sendDigital(keycode, true)
                // Wait for the release.
                while (true) {
                    val next = awaitPointerEvent()
                    val nc = next.changes.firstOrNull { it.id == change.id }
                    if (nc == null || !nc.pressed) break
                }
                onPressedChange(false)
                sendDigital(keycode, false)
            }
        }
    }

/** Edit-mode gestures — tap selects the widget (so the toolbar can
 *  expose a size slider for it), pan moves, pinch resizes.
 *
 *  Two pointerInputs because Compose dispatches the same touch events
 *  to both: the onPress fires on initial pointer-down (used for
 *  selection), and detectTransformGestures fires on movement past
 *  touch-slop (used for pan/pinch). They cooperate cleanly — onPress
 *  doesn't consume, so the transform handler still sees the same
 *  events.
 *
 *  Note: the `pointerInput(cfg.id)` key never changes, so the captured
 *  `cfg` here is frozen at first composition and stays stale across
 *  drags. Always read live state from TouchControls inside the
 *  transform lambda. */
private fun Modifier.editGestures(cfg: TouchButtonCfg): Modifier =
    pointerInput(cfg.id, "press") {
        detectTapGestures(
            onPress = { TouchControls.selectedButton.value = cfg.id },
        )
    }.pointerInput(cfg.id) {
        detectTransformGestures(panZoomLock = false) { _, pan, zoom, _ ->
            val overlay = OverlayDims.last ?: return@detectTransformGestures
            TouchControls.updateButton(cfg.id) { current ->
                val newX = (current.xFrac + pan.x / overlay.widthPx).coerceIn(0.02f, 0.98f)
                val newY = (current.yFrac + pan.y / overlay.heightPx).coerceIn(0.02f, 0.98f)
                val newSize = (current.sizeDp * zoom).coerceIn(28f, 220f)
                current.copy(xFrac = newX, yFrac = newY, sizeDp = newSize)
            }
        }
    }

private object OverlayDims {
    @Volatile var last: Dims? = null
    data class Dims(val widthPx: Float, val heightPx: Float)
}

/* -------------------------------------------------------------------- */
/*  Edit-mode adornments + toolbar                                       */
/* -------------------------------------------------------------------- */

/** Outline drawn over every widget while in edit mode. Brighter +
 *  thicker for the currently-selected widget so the user can confirm
 *  which one the toolbar size slider operates on. */
@Composable
private fun EditAdornment(id: TouchButtonId? = null) {
    val isSelected = id != null && TouchControls.selectedButton.value == id
    val color = if (isSelected) Color(0xFFFFD33A) else Colors.pasx2_blue
    val width = if (isSelected) 3.dp else 2.dp
    Box(
        Modifier
            .fillMaxSize()
            .border(width, color, RoundedCornerShape(8.dp)),
    )
}

@Composable
private fun EditToolbar(modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .clip(RoundedCornerShape(12.dp))
            .background(Color(0xCC000000))
            .padding(horizontal = 12.dp, vertical = 8.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        // Action chips up top — save commits the live layout into the
        // active profile, discard reverts to the saved version, reset
        // restores the default, profiles opens the picker.
        Row(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            ToolbarChip("Save") {
                TouchControls.saveLiveLayoutToActive()
                TouchControls.editMode.value = false
            }
            ToolbarChip("Discard") {
                TouchControls.discardEdits()
                TouchControls.editMode.value = false
            }
            ToolbarChip("Reset") { TouchControls.resetActiveToDefault() }
            ToolbarChip("Profiles") { TouchControls.profileDialogOpen.value = true }
        }
        // Opacity slider — controls the live HUD alpha so the user sees
        // the change immediately while editing. Range 0.20..1.00 mirrors
        // TouchControls.setOpacity's clamp.
        Row(
            // Wrap-content width + Column's CenterHorizontally alignment
            // centers the alpha bar horizontally inside the toolbar
            // regardless of how wide the action-chip row above ends up.
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text("α", color = Color(0xFFAAAAAA), fontSize = 12.sp)
            androidx.compose.material3.Slider(
                value = TouchControls.opacity.value,
                onValueChange = { TouchControls.setOpacity(it) },
                valueRange = 0.20f..1.0f,
                modifier = Modifier
                    .width(280.dp)
                    .height(28.dp),
                colors = androidx.compose.material3.SliderDefaults.colors(
                    thumbColor = Colors.pasx2_blue,
                    activeTrackColor = Colors.pasx2_blue,
                    inactiveTrackColor = Color(0xFF333344),
                ),
            )
            Text(
                "${(TouchControls.opacity.value * 100).toInt()}%",
                color = Color(0xFFAAAAAA),
                fontSize = 11.sp,
                modifier = Modifier.width(40.dp),
            )
        }
        // Size slider — only present when a widget is selected. Lets
        // the user resize tiny buttons (L3/R3, Start/Select) that are
        // awkward to pinch-zoom directly. Selection is cleared by
        // tapping the dim backdrop.
        val selected = TouchControls.selectedButton.value
        val selectedCfg = if (selected != null)
            TouchControls.activeLayout.value.buttons.firstOrNull { it.id == selected }
        else null
        if (selectedCfg != null) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(
                    selectedCfg.id.label + " size",
                    color = Color(0xFFFFD33A),
                    fontSize = 12.sp,
                    fontWeight = FontWeight.SemiBold,
                )
                androidx.compose.material3.Slider(
                    value = selectedCfg.sizeDp,
                    onValueChange = { newSize ->
                        TouchControls.updateButton(selectedCfg.id) {
                            it.copy(sizeDp = newSize.coerceIn(28f, 220f))
                        }
                    },
                    valueRange = 28f..220f,
                    modifier = Modifier
                        .width(240.dp)
                        .height(28.dp),
                    colors = androidx.compose.material3.SliderDefaults.colors(
                        thumbColor = Color(0xFFFFD33A),
                        activeTrackColor = Color(0xFFFFD33A),
                        inactiveTrackColor = Color(0xFF444433),
                    ),
                )
                Text(
                    "${selectedCfg.sizeDp.toInt()}dp",
                    color = Color(0xFFAAAAAA),
                    fontSize = 11.sp,
                    modifier = Modifier.width(48.dp),
                )
            }
        }
    }
}

@Composable
private fun ToolbarChip(label: String, onClick: () -> Unit) {
    Box(
        Modifier
            .clip(RoundedCornerShape(8.dp))
            .background(Color(0xFF1F1F2C))
            .clickable(onClick = onClick)
            .padding(horizontal = 10.dp, vertical = 6.dp),
    ) {
        Text(label, color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
    }
}

/* -------------------------------------------------------------------- */
/*  Profile picker                                                       */
/* -------------------------------------------------------------------- */

@Composable
private fun ProfilePicker(onDismiss: () -> Unit) {
    var newName by remember { mutableStateOf("") }
    Box(
        Modifier
            .fillMaxSize()
            .background(Color(0xCC000000))
            .clickable(onClick = onDismiss),
        contentAlignment = Alignment.Center,
    ) {
        Column(
            modifier = Modifier
                .width(360.dp)
                .clip(RoundedCornerShape(8.dp))
                .background(Color(0xFF1A1A24))
                // Eat taps inside the dialog so onDismiss only fires on
                // the backdrop. clickable with no onClick — Compose
                // requires an onClick lambda — so use an empty lambda.
                .clickable(enabled = true, onClick = {})
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Text(
                "Touch Control Profiles",
                color = Color.White,
                fontSize = 16.sp,
                fontWeight = FontWeight.Bold,
            )
            Spacer(Modifier.height(4.dp))
            for (p in TouchControls.profiles) {
                val active = p.name == TouchControls.activeProfileName.value
                Row(
                    Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(6.dp))
                        .background(if (active) Colors.pasx2_blue.copy(alpha = 0.35f) else Color(0xFF202030))
                        .clickable { TouchControls.switchProfile(p.name) }
                        .padding(horizontal = 10.dp, vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        if (active) "● ${p.name}" else "  ${p.name}",
                        color = Color.White,
                        fontSize = 13.sp,
                        modifier = Modifier.weight(1f),
                    )
                    if (TouchControls.profiles.size > 1) {
                        Text(
                            "Delete",
                            color = Color(0xFFFF6B6B),
                            fontSize = 11.sp,
                            modifier = Modifier
                                .clickable { TouchControls.deleteProfile(p.name) }
                                .padding(horizontal = 6.dp),
                        )
                    }
                }
            }
            Spacer(Modifier.height(8.dp))
            Text("Save current layout as new profile:", color = Color(0xFFAAAAAA), fontSize = 12.sp)
            Row(
                Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                androidx.compose.material3.OutlinedTextField(
                    value = newName,
                    onValueChange = { newName = it },
                    singleLine = true,
                    placeholder = { Text("Profile name", color = Color(0xFF888888)) },
                    colors = androidx.compose.material3.OutlinedTextFieldDefaults.colors(
                        focusedTextColor = Color.White,
                        unfocusedTextColor = Color.White,
                        focusedBorderColor = Colors.pasx2_blue,
                        unfocusedBorderColor = Color(0xFF444455),
                    ),
                    modifier = Modifier.weight(1f),
                )
                Box(
                    Modifier
                        .clip(RoundedCornerShape(6.dp))
                        .background(Colors.pasx2_blue)
                        .clickable(enabled = newName.isNotBlank()) {
                            TouchControls.saveAsNewProfile(newName)
                            newName = ""
                        }
                        .padding(horizontal = 12.dp, vertical = 10.dp),
                ) {
                    Text("Save As", color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
                }
            }
            Spacer(Modifier.height(6.dp))
            Box(
                Modifier
                    .align(Alignment.End)
                    .clip(RoundedCornerShape(6.dp))
                    .background(Color(0xFF333344))
                    .clickable(onClick = onDismiss)
                    .padding(horizontal = 16.dp, vertical = 8.dp),
            ) {
                Text("Close", color = Color.White, fontSize = 12.sp)
            }
        }
    }
}
