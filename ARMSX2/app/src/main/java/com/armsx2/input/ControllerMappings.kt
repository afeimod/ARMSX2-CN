package com.armsx2.input

import android.view.KeyEvent
import androidx.compose.runtime.mutableStateOf
import com.armsx2.Main

object ControllerMappings {
    data class Action(
        val id: String,
        val label: String,
        val targetKeyCode: Int,
        val defaultPhysicalKeyCode: Int,
    )

    val actions = listOf(
        Action("dpad_up", "D-Pad Up", KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_UP),
        Action("dpad_down", "D-Pad Down", KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_DOWN),
        Action("dpad_left", "D-Pad Left", KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_LEFT),
        Action("dpad_right", "D-Pad Right", KeyEvent.KEYCODE_DPAD_RIGHT, KeyEvent.KEYCODE_DPAD_RIGHT),
        Action("cross", "Cross", KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_A),
        Action("circle", "Circle", KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_B),
        Action("square", "Square", KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_X),
        Action("triangle", "Triangle", KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_Y),
        Action("l1", "L1", KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_L1),
        Action("r1", "R1", KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_R1),
        Action("l2", "L2", KeyEvent.KEYCODE_BUTTON_L2, KeyEvent.KEYCODE_BUTTON_L2),
        Action("r2", "R2", KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_BUTTON_R2),
        Action("l3", "L3", KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBL),
        Action("r3", "R3", KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_BUTTON_THUMBR),
        Action("select", "Select", KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_SELECT),
        Action("start", "Start", KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_START),
    )

    private const val KEY_PREFIX = "pad.map."

    fun physicalFor(action: Action): Int =
        Main.prefs.getInt(KEY_PREFIX + action.id, action.defaultPhysicalKeyCode)

    fun labelForKey(keyCode: Int): String = when (keyCode) {
        KeyEvent.KEYCODE_DPAD_UP -> "D-Pad Up"
        KeyEvent.KEYCODE_DPAD_DOWN -> "D-Pad Down"
        KeyEvent.KEYCODE_DPAD_LEFT -> "D-Pad Left"
        KeyEvent.KEYCODE_DPAD_RIGHT -> "D-Pad Right"
        KeyEvent.KEYCODE_BUTTON_A -> "Button A"
        KeyEvent.KEYCODE_BUTTON_B -> "Button B"
        KeyEvent.KEYCODE_BUTTON_X -> "Button X"
        KeyEvent.KEYCODE_BUTTON_Y -> "Button Y"
        KeyEvent.KEYCODE_BUTTON_L1 -> "L1"
        KeyEvent.KEYCODE_BUTTON_R1 -> "R1"
        KeyEvent.KEYCODE_BUTTON_L2 -> "L2"
        KeyEvent.KEYCODE_BUTTON_R2 -> "R2"
        KeyEvent.KEYCODE_BUTTON_THUMBL -> "L3"
        KeyEvent.KEYCODE_BUTTON_THUMBR -> "R3"
        KeyEvent.KEYCODE_BUTTON_SELECT -> "Select"
        KeyEvent.KEYCODE_BUTTON_START -> "Start"
        else -> KeyEvent.keyCodeToString(keyCode).removePrefix("KEYCODE_")
    }

    fun bind(action: Action, physicalKeyCode: Int) {
        Main.prefs.edit().putInt(KEY_PREFIX + action.id, physicalKeyCode).apply()
    }

    fun reset() {
        val edit = Main.prefs.edit()
        actions.forEach { edit.remove(KEY_PREFIX + it.id) }
        edit.apply()
    }

    fun targetForPhysical(physicalKeyCode: Int): Int? =
        actions.firstOrNull { physicalFor(it) == physicalKeyCode }?.targetKeyCode

    // ---- System hotkeys (menu / quick save / quick load) -----------------
    // Physical buttons bound to app actions, NOT forwarded to the PS2. Handled in
    // Main.dispatchKeyEvent (so they can catch KEYCODE_BACK / back-paddle keys the
    // back dispatcher would otherwise swallow). KEYCODE_UNKNOWN = unbound.
    enum class SysHotkey(val prefKey: String, val label: String) {
        MENU("pad.menu.keycode", "Menu / Pause"),
        SAVE_STATE("pad.savestate.keycode", "Quick Save State"),
        LOAD_STATE("pad.loadstate.keycode", "Quick Load State"),
        CYCLE_SLOT("pad.cycleslot.keycode", "Cycle Save Slot"),
        TEXTURE_DUMP("pad.texdump.keycode", "Toggle Texture Dumping"),
        FAST_FORWARD("pad.fastforward.keycode", "Fast Forward (hold)"),
        RES_UP("pad.resup.keycode", "Increase Resolution"),
        RES_DOWN("pad.resdown.keycode", "Decrease Resolution"),
        ACHIEVEMENTS("pad.achievements.keycode", "Open Achievements"),
        CLOSE_GAME("pad.closegame.keycode", "Close Game"),
        // Hold-type binding: while the bound button is held, pressure-capable PS2
        // buttons report a soft (~50%) press. Handled as a HOLD in
        // Main.dispatchKeyEvent (sets TouchControls.pressureModifierHeld), not as a
        // one-shot action like the others.
        PRESSURE_MOD("pad.pressuremod.keycode", "Pressure Modifier (hold)"),
    }

    // A hotkey is either a single button or a two-button combo. The main key is
    // stored under prefKey; an optional modifier (held while the main key is
    // pressed) under prefKey + MOD_SUFFIX. UNKNOWN modifier = single-button.
    private const val MOD_SUFFIX = ".mod"

    fun hotkeyCode(h: SysHotkey): Int =
        Main.prefs.getInt(h.prefKey, KeyEvent.KEYCODE_UNKNOWN)

    /** Modifier button that must be held with [hotkeyCode], or UNKNOWN for none. */
    fun hotkeyModCode(h: SysHotkey): Int =
        Main.prefs.getInt(h.prefKey + MOD_SUFFIX, KeyEvent.KEYCODE_UNKNOWN)

    /** Bind a single-button hotkey (clears any modifier). */
    fun bindHotkey(h: SysHotkey, physicalKeyCode: Int) {
        Main.prefs.edit()
            .putInt(h.prefKey, physicalKeyCode)
            .putInt(h.prefKey + MOD_SUFFIX, KeyEvent.KEYCODE_UNKNOWN)
            .apply()
    }

    /** Bind a two-button combo: [modCode] held + [keyCode] pressed. */
    fun bindHotkeyCombo(h: SysHotkey, modCode: Int, keyCode: Int) {
        Main.prefs.edit()
            .putInt(h.prefKey, keyCode)
            .putInt(h.prefKey + MOD_SUFFIX, modCode)
            .apply()
    }

    fun clearHotkey(h: SysHotkey) {
        Main.prefs.edit()
            .putInt(h.prefKey, KeyEvent.KEYCODE_UNKNOWN)
            .putInt(h.prefKey + MOD_SUFFIX, KeyEvent.KEYCODE_UNKNOWN)
            .apply()
    }

    /** Human-readable binding, e.g. "Select + R1" or "L1", or "" if unbound. */
    fun hotkeyLabel(h: SysHotkey): String {
        val key = hotkeyCode(h)
        if (key == KeyEvent.KEYCODE_UNKNOWN) return ""
        val mod = hotkeyModCode(h)
        return if (mod == KeyEvent.KEYCODE_UNKNOWN) labelForKey(key)
        else "${labelForKey(mod)} + ${labelForKey(key)}"
    }

    /** Single-button match (combos excluded). Used by the frontend MENU shortcut. */
    fun hotkeyFor(physicalKeyCode: Int): SysHotkey? {
        if (physicalKeyCode == KeyEvent.KEYCODE_UNKNOWN) return null
        return SysHotkey.values().firstOrNull {
            hotkeyCode(it) == physicalKeyCode && hotkeyModCode(it) == KeyEvent.KEYCODE_UNKNOWN
        }
    }

    /** Combo-aware match for the just-pressed [keyCode] given the set of
     *  currently-held physical keys. Combos (modifier held) win over a plain
     *  single binding on the same key, so e.g. Select+R1 fires its action
     *  instead of a bare-R1 binding while Select is held. */
    fun matchHotkey(keyCode: Int, heldKeys: Set<Int>): SysHotkey? {
        if (keyCode == KeyEvent.KEYCODE_UNKNOWN) return null
        SysHotkey.values().firstOrNull {
            hotkeyCode(it) == keyCode &&
                hotkeyModCode(it) != KeyEvent.KEYCODE_UNKNOWN &&
                heldKeys.contains(hotkeyModCode(it))
        }?.let { return it }
        return SysHotkey.values().firstOrNull {
            hotkeyCode(it) == keyCode && hotkeyModCode(it) == KeyEvent.KEYCODE_UNKNOWN
        }
    }

    // True while the Pad tab is waiting for a button to bind. Main.dispatchKeyEvent
    // checks this and lets EVERY key fall through to Compose's onPreviewKeyEvent so
    // any button — including B/A/Y/D-pad/L1/R1 the overlay nav would otherwise
    // consume (B = exit) — can be captured. Normal nav resumes when it clears.
    val padCapturing = mutableStateOf(false)

    // Capture bridge: the Hotkeys tab calls [beginHotkeyCapture]; the next
    // button(s) seen by Main.dispatchKeyEvent are bound to it. Press one button
    // for a single bind, or two together for a combo. Observed for UI feedback.
    val captureHotkey = mutableStateOf<SysHotkey?>(null)

    /** Ordered buffer of buttons pressed during an active capture (≤2 used). */
    val captureKeys = mutableListOf<Int>()

    /** Start capturing a (re)binding for [h]. */
    fun beginHotkeyCapture(h: SysHotkey) {
        captureKeys.clear()
        captureHotkey.value = h
    }

    /** End the current capture session. */
    fun endHotkeyCapture() {
        captureKeys.clear()
        captureHotkey.value = null
        hotkeyBindTick.value++
    }

    /** Bumped after a (re)bind so observing UI recomposes. */
    val hotkeyBindTick = mutableStateOf(0)
}
