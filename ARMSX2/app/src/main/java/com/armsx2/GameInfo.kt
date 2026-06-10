package com.armsx2

import android.net.Uri

/**
 * One row in the games-list screen. Today the title/serial come from
 * filename parsing — game titles like "Final Fantasy X (USA) [SLUS-20312]"
 * are common dump conventions. compatibility is left at 0 (no stars filled)
 * until we add a gamedb JNI bridge.
 *
 * `platform` distinguishes PS1 ("ps1") from PS2 ("ps2") so we hit the
 * right cover repo: xlenore/ps2-covers vs xlenore/psx-covers. Native
 * (getGameSerialFromFd) tags its return with the platform when SYSTEM.CNF
 * is parseable; filename-only fallback defaults to "ps2".
 */
enum class GamePlatform(val key: String) {
    PS2("ps2"),
    PS1("ps1");

    companion object {
        fun fromKey(s: String?): GamePlatform =
            if (s == "ps1") PS1 else PS2
    }
}

data class GameInfo(
    val uri: Uri,
    val title: String,
    val serial: String?,
    val compatibility: Int = 0,    // 0..5 (TODO: pull from gamedb)
    val extension: String = "",    // upper-case container ext, e.g. "ISO", "CHD"
    val platform: GamePlatform = GamePlatform.PS2,
) {
    val coverUrl: String? get() = serial?.let { s ->
        when (platform) {
            GamePlatform.PS2 ->
                "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/$s.jpg"
            GamePlatform.PS1 ->
                "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default/$s.jpg"
        }
    }
}

/**
 * Best-effort serial extractor. Recognized dump conventions:
 *   "Game (USA) [SLUS-20312].iso"      → SLUS-20312
 *   "Game (USA) [SLUS_203.12].iso"     → SLUS-20312
 *   "SCUS_972.28 - Game.iso"           → SCUS-97228
 *   "slus_203.12.iso"                  → SLUS-20312
 *
 * The pattern matches 4 letters + optional separator + 3 digits + optional
 * dot + 2 digits, normalized to "AAAA-NNNNN" upper-case.
 */
object FilenameParser {
    private val serialRegex = Regex("""([A-Za-z]{4})[\s_-]?(\d{3})\.?(\d{2})""")
    private val tagsRegex = Regex("""[\[(].*?[\])]""")
    private val whitespaceRegex = Regex("""\s+""")

    fun parse(filename: String): Pair<String, String?> {
        val withoutExt = filename.substringBeforeLast('.')
        val match = serialRegex.find(withoutExt)
        val serial = match?.let {
            "${it.groupValues[1].uppercase()}-${it.groupValues[2]}${it.groupValues[3]}"
        }
        // Strip the matched serial token + any [region] / (lang) tags so the
        // displayed title is the game name rather than the full filename.
        var title = withoutExt
        if (match != null) title = title.replace(match.value, "")
        title = title.replace(tagsRegex, "")
            .replace(whitespaceRegex, " ")
            .trim(' ', '-', '_', '.')
        if (title.isEmpty()) title = withoutExt
        return title to serial
    }
}
