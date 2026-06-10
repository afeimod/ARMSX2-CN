package com.armsx2.ui.touch

import android.view.KeyEvent
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import com.armsx2.Main
import org.json.JSONArray
import org.json.JSONObject

/**
 * On-screen touch controls — state, persistence, and the runtime
 * input-mode latch.
 *
 * Three storage tiers in Main.prefs (single "ARMSX2" SharedPreferences):
 *   - `touch.profiles`            JSON array of {name, layout}
 *   - `touch.active`              Currently-selected profile name
 *
 * The "active layout" is always a copy of the active profile's layout —
 * edits live there until the user explicitly saves them back into a
 * profile (Save / Save As New).
 */
object TouchControls {
    private const val KEY_PROFILES = "touch.profiles"
    private const val KEY_ACTIVE = "touch.active"
    private const val KEY_OPACITY = "touch.opacity"

    /** Visible to the user. False when a controller is being used (latched
     *  off in onControllerInputDetected); flipped back on by any screen
     *  touch via onSurfaceTouched. Default true so first-run users see
     *  the controls. */
    val visible = mutableStateOf(true)

    /** Edit mode — buttons are draggable + resizable, JNI pad writes are
     *  suppressed. Toggled from InGameOverlay's "Edit Touch Controls"
     *  row. */
    val editMode = mutableStateOf(false)

    /** Currently-selected widget in edit mode. When non-null, the edit
     *  toolbar exposes a size slider for the selected widget — useful
     *  for resizing tiny buttons (L3/R3, Start/Select) that are
     *  awkward to pinch-zoom. Tap a widget to select; tap the dim
     *  backdrop to deselect. */
    val selectedButton = mutableStateOf<TouchButtonId?>(null)

    /** Profile picker / save-as dialog shown over the editor. */
    val profileDialogOpen = mutableStateOf(false)

    /** All saved profiles. Mutated via mutators below so observers
     *  recompose. */
    val profiles = mutableStateListOf<TouchProfile>()

    /** Name of the currently-active profile. Persisted. */
    val activeProfileName = mutableStateOf("Default")

    /** Live layout being rendered + edited. Diverges from the saved
     *  profile while editing; "Save" commits it back, "Discard" reloads. */
    val activeLayout = mutableStateOf(TouchLayout.default())

    /** Master opacity 0.20..1.00. Persisted. */
    val opacity = mutableStateOf(0.55f)

    /** Set true once load() has run — used to avoid clobbering disk state
     *  on first composition. */
    private var loaded = false

    fun ensureLoaded() {
        if (loaded) return
        loaded = true
        load()
    }

    private fun load() {
        val raw = Main.prefs.getString(KEY_PROFILES, null)
        val list = mutableListOf<TouchProfile>()
        if (raw != null) {
            runCatching {
                val arr = JSONArray(raw)
                for (i in 0 until arr.length()) {
                    val obj = arr.getJSONObject(i)
                    list.add(TouchProfile.fromJson(obj))
                }
            }
        }
        if (list.isEmpty()) {
            list.add(TouchProfile("Default", TouchLayout.default()))
        }
        profiles.clear()
        profiles.addAll(list)

        val active = Main.prefs.getString(KEY_ACTIVE, list.first().name) ?: list.first().name
        activeProfileName.value = active
        val match = list.firstOrNull { it.name == active } ?: list.first()
        activeLayout.value = match.layout.copy()
        opacity.value = Main.prefs.getFloat(KEY_OPACITY, 0.55f).coerceIn(0.20f, 1.0f)
    }

    private fun persist() {
        val arr = JSONArray()
        for (p in profiles) arr.put(p.toJson())
        Main.prefs.edit()
            .putString(KEY_PROFILES, arr.toString())
            .putString(KEY_ACTIVE, activeProfileName.value)
            .putFloat(KEY_OPACITY, opacity.value)
            .apply()
    }

    /** Replace the layout of the active profile with the live edit state. */
    fun saveLiveLayoutToActive() {
        val idx = profiles.indexOfFirst { it.name == activeProfileName.value }
        if (idx >= 0) {
            profiles[idx] = profiles[idx].copy(layout = activeLayout.value.copy())
            persist()
        }
        selectedButton.value = null
    }

    /** Persist the live edit state under a new profile name. If the name
     *  collides, the existing profile is overwritten. Switches to the new
     *  profile. */
    fun saveAsNewProfile(name: String) {
        val trimmed = name.trim().ifEmpty { return }
        val newProf = TouchProfile(trimmed, activeLayout.value.copy())
        val existing = profiles.indexOfFirst { it.name == trimmed }
        if (existing >= 0) profiles[existing] = newProf
        else profiles.add(newProf)
        activeProfileName.value = trimmed
        persist()
    }

    fun switchProfile(name: String) {
        val match = profiles.firstOrNull { it.name == name } ?: return
        activeProfileName.value = name
        activeLayout.value = match.layout.copy()
        persist()
    }

    fun deleteProfile(name: String) {
        if (profiles.size <= 1) return  // never delete the last profile
        val idx = profiles.indexOfFirst { it.name == name }
        if (idx < 0) return
        profiles.removeAt(idx)
        if (activeProfileName.value == name) {
            val fallback = profiles.first()
            activeProfileName.value = fallback.name
            activeLayout.value = fallback.layout.copy()
        }
        persist()
    }

    fun resetActiveToDefault() {
        activeLayout.value = TouchLayout.default()
    }

    /** Reload the live layout from the saved active profile, discarding
     *  any unsaved edits. */
    fun discardEdits() {
        val match = profiles.firstOrNull { it.name == activeProfileName.value }
        if (match != null) activeLayout.value = match.layout.copy()
        selectedButton.value = null
    }

    fun setOpacity(o: Float) {
        opacity.value = o.coerceIn(0.20f, 1.0f)
        persist()
    }

    /** Update a single button in the live layout. */
    fun updateButton(id: TouchButtonId, transform: (TouchButtonCfg) -> TouchButtonCfg) {
        val current = activeLayout.value
        val newButtons = current.buttons.map { if (it.id == id) transform(it) else it }
        activeLayout.value = current.copy(buttons = newButtons)
    }

    /** Latched off the touch controls when a controller key/axis fires.
     *  Idempotent — only writes state when a flip is needed. */
    fun onControllerInputDetected() {
        if (visible.value) visible.value = false
    }

    /** Latched on by any pointer-down on the surface so a controller user
     *  who touches the screen sees the controls again. */
    fun onSurfaceTouched() {
        if (!visible.value) visible.value = true
    }
}

/** Stable id for a touch widget. The keycode is the canonical primary
 *  keycode the widget emits (digital buttons emit one code; the DPad +
 *  sticks emit four codes derived from the four cardinal directions —
 *  the keycode here is the "up" / first code for serialization id
 *  purposes only, the rendering layer maps internally). */
enum class TouchButtonId(val label: String, val keycode: Int, val kind: Kind) {
    CROSS("✕", KeyEvent.KEYCODE_BUTTON_A, Kind.FACE),
    CIRCLE("○", KeyEvent.KEYCODE_BUTTON_B, Kind.FACE),
    SQUARE("□", KeyEvent.KEYCODE_BUTTON_X, Kind.FACE),
    TRIANGLE("△", KeyEvent.KEYCODE_BUTTON_Y, Kind.FACE),
    L1("L1", KeyEvent.KEYCODE_BUTTON_L1, Kind.SHOULDER),
    R1("R1", KeyEvent.KEYCODE_BUTTON_R1, Kind.SHOULDER),
    L2("L2", KeyEvent.KEYCODE_BUTTON_L2, Kind.SHOULDER),
    R2("R2", KeyEvent.KEYCODE_BUTTON_R2, Kind.SHOULDER),
    START("Start", KeyEvent.KEYCODE_BUTTON_START, Kind.MENU),
    SELECT("Select", KeyEvent.KEYCODE_BUTTON_SELECT, Kind.MENU),
    // L3 / R3 — separate stick-CLICK buttons (the press-the-thumbstick
    // action). The L_STICK / R_STICK widgets only emit axis movement;
    // these emit the THUMBL / THUMBR keycodes.
    L3("L3", KeyEvent.KEYCODE_BUTTON_THUMBL, Kind.MENU),
    R3("R3", KeyEvent.KEYCODE_BUTTON_THUMBR, Kind.MENU),
    DPAD("D-Pad", KeyEvent.KEYCODE_DPAD_UP, Kind.DPAD),
    L_STICK("L-Stick", 110, Kind.STICK),
    R_STICK("R-Stick", 120, Kind.STICK),
    // Invisible long-press hotspot that opens the in-game pause overlay.
    // Replaced the old long-press-anywhere-on-the-surface gesture, which
    // fired on accidental presses in empty space mid-game. Emits no pad
    // keycode (0 = unused); renders nothing in play mode, shows an
    // outlined "PAUSE" box in edit mode so it can be moved/resized.
    PAUSE("Pause", 0, Kind.PAUSE);

    enum class Kind { FACE, SHOULDER, MENU, DPAD, STICK, PAUSE }
}

/** Position + size for a single widget. xFrac / yFrac are anchor-point
 *  fractions of screen width/height (0..1, 0,0 = top-left). sizeDp is
 *  the widget's outer diameter / largest side. */
data class TouchButtonCfg(
    val id: TouchButtonId,
    val xFrac: Float,
    val yFrac: Float,
    val sizeDp: Float,
    val enabled: Boolean = true,
) {
    fun toJson(): JSONObject = JSONObject().apply {
        put("id", id.name)
        put("x", xFrac.toDouble())
        put("y", yFrac.toDouble())
        put("size", sizeDp.toDouble())
        put("on", enabled)
    }

    companion object {
        fun fromJson(json: JSONObject): TouchButtonCfg? {
            val idName = json.optString("id", "") ?: ""
            val id = runCatching { TouchButtonId.valueOf(idName) }.getOrNull() ?: return null
            return TouchButtonCfg(
                id = id,
                xFrac = json.optDouble("x", 0.5).toFloat().coerceIn(0f, 1f),
                yFrac = json.optDouble("y", 0.5).toFloat().coerceIn(0f, 1f),
                sizeDp = json.optDouble("size", 64.0).toFloat().coerceIn(28f, 220f),
                enabled = json.optBoolean("on", true),
            )
        }
    }
}

data class TouchLayout(val buttons: List<TouchButtonCfg>) {
    fun toJson(): JSONObject = JSONObject().apply {
        val arr = JSONArray()
        for (b in buttons) arr.put(b.toJson())
        put("buttons", arr)
    }

    companion object {
        fun fromJson(json: JSONObject): TouchLayout {
            val arr = json.optJSONArray("buttons") ?: return default()
            val list = mutableListOf<TouchButtonCfg>()
            for (i in 0 until arr.length()) {
                val obj = arr.optJSONObject(i) ?: continue
                TouchButtonCfg.fromJson(obj)?.let { list.add(it) }
            }
            // If the persisted layout is missing any new buttons added
            // since it was saved, splice in their defaults so the user
            // gets the new widget without having to reset. Matches
            // Settings.kt's optKey + fallback pattern.
            val have = list.map { it.id }.toSet()
            val merged = list + default().buttons.filter { it.id !in have }
            return TouchLayout(merged)
        }

        /** Landscape-tuned default. Coordinates assume a 16:9-ish layout
         *  and are clamped to safe areas on edges. The user can drag in
         *  edit mode to fit their device — this is just the starting
         *  point. */
        fun default(): TouchLayout = TouchLayout(
            buttons = listOf(
                // DPad cluster — lower left of screen, above L-stick room
                TouchButtonCfg(TouchButtonId.DPAD,     0.10f, 0.55f, 150f),
                // Face button diamond — lower right
                TouchButtonCfg(TouchButtonId.TRIANGLE, 0.86f, 0.45f, 58f),
                TouchButtonCfg(TouchButtonId.SQUARE,   0.80f, 0.55f, 58f),
                TouchButtonCfg(TouchButtonId.CIRCLE,   0.92f, 0.55f, 58f),
                TouchButtonCfg(TouchButtonId.CROSS,    0.86f, 0.65f, 58f),
                // Shoulders stacked vertically on each side: L2 / R2 on
                // top (further trigger), L1 / R1 directly below them.
                // Gap is ~16% of screen height — on a 390dp landscape
                // height that's 62dp center-to-center, ~6dp visible gap
                // between the 56dp buttons. Tight enough to read as a
                // pair without overlapping.
                TouchButtonCfg(TouchButtonId.L2,       0.08f, 0.10f, 56f),
                TouchButtonCfg(TouchButtonId.L1,       0.08f, 0.23f, 56f),
                TouchButtonCfg(TouchButtonId.R2,       0.92f, 0.10f, 56f),
                TouchButtonCfg(TouchButtonId.R1,       0.92f, 0.23f, 56f),
                // Start / Select centered at the bottom
                TouchButtonCfg(TouchButtonId.SELECT,   0.45f, 0.92f, 48f),
                TouchButtonCfg(TouchButtonId.START,    0.55f, 0.92f, 48f),
                // Analog sticks — bottom inside, between DPad/face cluster
                // and the center, so thumb travel is short.
                TouchButtonCfg(TouchButtonId.L_STICK,  0.28f, 0.80f, 130f),
                TouchButtonCfg(TouchButtonId.R_STICK,  0.72f, 0.80f, 130f),
                // L3 / R3 stick-click buttons, anchored at the lower
                // outside corner of each thumbstick (away from the
                // screen center so the user's hand resting on the stick
                // doesn't accidentally press them).
                TouchButtonCfg(TouchButtonId.L3,       0.18f, 0.93f, 42f),
                TouchButtonCfg(TouchButtonId.R3,       0.82f, 0.93f, 42f),
                // Invisible pause hotspot — dead center between the DPad
                // (0.10) and the face diamond (0.86), on their shared row,
                // clear of the sticks (y 0.80) and Start/Select (y 0.92).
                TouchButtonCfg(TouchButtonId.PAUSE,    0.48f, 0.50f, 120f),
            ),
        )
    }
}

data class TouchProfile(val name: String, val layout: TouchLayout) {
    fun toJson(): JSONObject = JSONObject().apply {
        put("name", name)
        put("layout", layout.toJson())
    }

    companion object {
        fun fromJson(json: JSONObject): TouchProfile {
            return TouchProfile(
                name = json.optString("name", "Profile"),
                layout = json.optJSONObject("layout")?.let { TouchLayout.fromJson(it) }
                    ?: TouchLayout.default(),
            )
        }
    }
}
