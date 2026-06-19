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
/**
 * Where overlay setting changes land. The overlay header picks one at
 * runtime via a switch; default is [Game] when a game is loaded,
 * [Global] otherwise.
 *
 *   - [Global] writes the full Settings to `config.global`. Affects every
 *     future game launch that doesn't have its own per-game override.
 *   - [Game] writes the SPARSE diff (only fields differing from current
 *     global) to `config.game.<serial>`. Global stays untouched; only
 *     this title sees the change.
 */
enum class SettingsScope { Global, Game }

object ConfigStore {
    private const val KEY_GLOBAL = "config.global"
    private const val KEY_BLEND_BASIC_MIGRATED = "config.migrated.blendBasic"
    private fun keyForGame(serial: String) = "config.game.$serial"

    fun loadGlobal(): Settings {
        val raw = Main.prefs.getString(KEY_GLOBAL, null) ?: return Settings()
        val parsed = try {
            Settings.fromJson(JSONObject(raw))
        } catch (_: Exception) {
            Settings()
        }
        val migrated = Main.prefs.getBoolean(KEY_BLEND_BASIC_MIGRATED, false)
        if (!migrated && parsed.accurateBlendingUnit == 4) {
            val updated = parsed.copy(accurateBlendingUnit = 1)
            Main.prefs.edit()
                .putBoolean(KEY_BLEND_BASIC_MIGRATED, true)
                .putString(KEY_GLOBAL, updated.toJson().toString())
                .commit()
            return updated
        }
        if (!migrated) {
            Main.prefs.edit().putBoolean(KEY_BLEND_BASIC_MIGRATED, true).apply()
        }
        return parsed
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

    /**
     * Single entry point for overlay tabs to persist a settings change.
     * Scope picks the storage tier; serial may be null (Global is the
     * only valid scope in that case).
     *
     * Game scope writes the sparse diff vs current global, so a later
     * global tweak still propagates to the field unless the user
     * explicitly overrode it for this title.
     */
    fun save(scope: SettingsScope, serial: String?, updated: Settings) {
        if (scope == SettingsScope.Game && serial != null) {
            val global = loadGlobal()
            val diff = Settings.diff(global, updated)
            saveOverrides(serial, diff)
        } else {
            saveGlobal(updated)
        }
    }
}
