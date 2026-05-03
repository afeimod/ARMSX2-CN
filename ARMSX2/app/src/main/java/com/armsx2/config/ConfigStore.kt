package com.armsx2.config

import com.armsx2.Main
import org.json.JSONObject

/**
 * Persistence + resolution for emu [Settings].
 *
 * Two storage tiers, both in `Main.prefs`:
 *   - **Global** under `config.global` — the user's baseline. Stored as a
 *     full Settings JSON.
 *   - **Per-game** under `config.game.<serial>` — sparse JSON containing
 *     ONLY the fields the user explicitly overrode for that title. Sparse
 *     storage means future global tweaks still flow through fields the
 *     user hasn't touched per-game.
 *
 * [resolveForGame] is the only method anyone outside the settings UI
 * should call. It returns the merged Settings to push at VM launch.
 *
 * No caching — load on demand. Writes are rare (user clicked Save) and
 * reads happen at game launch (once per launch). Both well within
 * SharedPreferences' overhead.
 */
object ConfigStore {
    private const val KEY_GLOBAL = "config.global"
    private fun keyForGame(serial: String) = "config.game.$serial"

    fun loadGlobal(): Settings {
        val raw = Main.prefs.getString(KEY_GLOBAL, null) ?: return Settings()
        return try {
            Settings.fromJson(JSONObject(raw))
        } catch (_: Exception) {
            Settings()
        }
    }

    fun saveGlobal(s: Settings) {
        Main.prefs.edit().putString(KEY_GLOBAL, s.toJson().toString()).apply()
    }

    /** Load the sparse per-game override blob, or null if there are none. */
    fun loadOverrides(serial: String): JSONObject? {
        val raw = Main.prefs.getString(keyForGame(serial), null) ?: return null
        return try {
            JSONObject(raw)
        } catch (_: Exception) {
            null
        }
    }

    fun saveOverrides(serial: String, overrides: JSONObject) {
        Main.prefs.edit().putString(keyForGame(serial), overrides.toString()).apply()
    }

    fun clearOverrides(serial: String) {
        Main.prefs.edit().remove(keyForGame(serial)).apply()
    }

    /**
     * Resolve effective Settings for a VM launch:
     *   per-game override (if present) ∘ global ∘ defaults.
     *
     * Pass null serial to skip the per-game tier (BIOS boots, anonymous
     * launches via Change Disc when no GameInfo was carried through).
     */
    fun resolveForGame(serial: String?): Settings {
        val global = loadGlobal()
        if (serial == null) return global
        val overrides = loadOverrides(serial) ?: return global
        return Settings.merge(global, overrides)
    }
}
