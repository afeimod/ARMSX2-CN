package com.armsx2

import android.net.Uri

/**
 * One row in the games-list screen. Today the title/serial come from
 * filename parsing — game titles like "Final Fantasy X (USA) [SLUS-20312]"
 * are common dump conventions. compatibility is left at 0 (no stars filled)
 * until we add a gamedb JNI bridge.
 *
 * `coverUrl` is computed from the serial using the xlenore/ps2-covers
 * repository (the user-specified source) when a serial was extractable;
 * null otherwise. Coil renders a placeholder for null URLs / 404s.
 */
data class GameInfo(
    val uri: Uri,
    val title: String,
    val serial: String?,
    val compatibility: Int = 0,    // 0..5 (TODO: pull from gamedb)
    val extension: String = "",    // upper-case container ext, e.g. "ISO", "CHD"
) {
    val coverUrl: String? get() = serial?.let {
        "https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/$it.jpg"
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
