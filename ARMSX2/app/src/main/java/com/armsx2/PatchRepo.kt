// SPDX-License-Identifier: GPL-3.0+
package com.armsx2

import android.util.Log
import kr.co.iefriends.pcsx2.HttpClient

/**
 * Online patch/cheat fetcher — the patch-side counterpart to [CustomDriver].
 *
 * Pulls a game's `.pnach` from the canonical PCSX2 patch database (the same
 * project the bundled `assets/resources/patches.zip` is built from) and parses
 * it into individual, toggleable [Entry]s for the in-app browser.
 *
 * Repo facts (verified): default branch is **main**; files are named
 * `<SERIAL>_<CRC>.pnach` (modern) or `<CRC>.pnach` (legacy) under `patches/`
 * (widescreen / fixes) and `cheats/` (cheat codes). Each patch/cheat is a
 * `[Section]` block with optional `comment=`/`author=` and one or more `patch=`
 * lines.
 *
 * Network calls are blocking ([HttpClient.doRequest]); call [fetchForGame] off
 * the main thread.
 */
object PatchRepo {
    private const val TAG = "PatchRepo"
    private const val RAW_BASE = "https://raw.githubusercontent.com/PCSX2/pcsx2_patches/main"
    private const val TREE_URL = "https://api.github.com/repos/PCSX2/pcsx2_patches/git/trees/main?recursive=1"
    private const val USER_AGENT = "ARMSX2"
    private val CRC_RE = Regex("^[0-9A-Fa-f]{8}$")
    private val SERIAL_RE = Regex("^[A-Z]{4}-\\d{5}$")
    private val TREE_PATH_RE = Regex("\"path\"\\s*:\\s*\"([^\"]+\\.pnach)\"")
    private val SECTION_RE = Regex("^\\s*\\[(.+?)]\\s*$")
    private val COMMENT_RE = Regex("^\\s*comment\\s*=\\s*(.+)$", RegexOption.IGNORE_CASE)
    private val GAMETITLE_RE = Regex("(?m)^\\s*gametitle\\s*=\\s*(.+)$")

    // Cached file listing of the whole patch repo (paths like
    // "patches/SLUS-20946_2C6BE434.pnach"). One API call per app session — the
    // raw .pnach fetches go through the CDN and don't count against the API
    // rate limit, so only this listing does.
    @Volatile private var treeCache: List<String>? = null

    /** A single toggleable patch/cheat. [body] is the verbatim `[Section]…`
     *  block (header + author/comment/patch lines) to write back out. [source]
     *  is "patches" or "cheats" — decides which folder it installs into. */
    data class Entry(
        val name: String,
        val description: String,
        val body: String,
        val source: String,
    )

    data class Result(
        val gametitle: String,
        val entries: List<Entry>,
        val error: String?,
        // Resolved game id for the matched files. [crc] comes from the DB
        // filename when browsing by serial (the disc isn't booted), so the
        // caller can name the saved .pnach so emucore loads it at boot.
        val serial: String = "",
        val crc: String = "",
    )

    /** Fetch + parse this game's patch/cheat entries. Tries `<SERIAL>_<CRC>`
     *  then `<CRC>` in both `patches/` and `cheats/`. */
    fun fetchForGame(serial: String?, crc: String): Result {
        val c = crc.trim().uppercase()
        if (!CRC_RE.matches(c))
            return Result("", emptyList(), "No game CRC yet — boot the game first.")

        val candidates = buildList {
            if (!serial.isNullOrBlank()) add("${serial}_$c")
            add(c)
        }
        var gametitle = ""
        val entries = mutableListOf<Entry>()
        for (dir in listOf("patches", "cheats")) {
            for (name in candidates) {
                val text = get("$RAW_BASE/$dir/$name.pnach") ?: continue
                val (gt, es) = parse(text, dir)
                if (gametitle.isEmpty()) gametitle = gt
                entries += es
                break // first existing file per folder wins
            }
        }
        if (entries.isEmpty())
            return Result("", emptyList(), "No patches or cheats in the database for ${serial ?: c}.")
        return Result(gametitle, entries, null, serial?.uppercase().orEmpty(), c)
    }

    /** Browse by serial only — for games picked from the library before being
     *  booted, where we have the serial but not the disc CRC. Looks the game up
     *  in the repo file tree to find its `<serial>_<crc>.pnach`; the CRC comes
     *  back in [Result.crc] so the caller can name the saved file correctly. */
    fun fetchForSerial(serial: String?): Result {
        val s = serial?.trim()?.uppercase()
        if (s.isNullOrBlank() || !SERIAL_RE.matches(s))
            return Result("", emptyList(), "This game has no serial to search the patch database with.")

        val tree = repoTree()
        if (tree.isEmpty())
            return Result("", emptyList(), "Couldn't reach the PCSX2 patch database. Check your connection.")

        var gametitle = ""
        var resolvedCrc = ""
        val entries = mutableListOf<Entry>()
        for (dir in listOf("patches", "cheats")) {
            val prefix = "$dir/${s}_"
            val match = tree.firstOrNull { it.startsWith(prefix, ignoreCase = true) } ?: continue
            val text = get("$RAW_BASE/$match") ?: continue
            val (gt, es) = parse(text, dir)
            if (gametitle.isEmpty()) gametitle = gt
            if (resolvedCrc.isEmpty()) {
                // "<dir>/<serial>_<CRC>[...].pnach" -> CRC
                resolvedCrc = match.substringAfterLast('/')
                    .removeSuffix(".pnach")
                    .substringAfter("${s}_", "")
                    .substringBefore('_')
                    .uppercase()
            }
            entries += es
        }
        if (entries.isEmpty())
            return Result("", emptyList(), "No patches or cheats in the database for $s.")
        return Result(gametitle, entries, null, s, resolvedCrc)
    }

    /** File listing of the whole patch repo, cached for the session. */
    private fun repoTree(): List<String> {
        treeCache?.let { return it }
        val json = get(TREE_URL) ?: return emptyList()
        val paths = TREE_PATH_RE.findAll(json).map { it.groupValues[1] }.toList()
        if (paths.isNotEmpty()) treeCache = paths
        return paths
    }

    /** Build a `.pnach` (gametitle + the given entries' blocks) for writing. */
    fun buildPnach(gametitle: String, entries: List<Entry>): String = buildString {
        if (gametitle.isNotEmpty()) append("gametitle=").append(gametitle).append("\n\n")
        entries.forEach { append(it.body.trimEnd()).append("\n\n") }
    }

    private fun get(url: String): String? {
        val resp = runCatching { HttpClient.doRequest(url, "GET", null, USER_AGENT, 15000) }
            .getOrElse { Log.w(TAG, "get $url failed: ${it.message}"); return null }
        if (resp.statusCode != 200 || resp.data.isEmpty()) {
            if (resp.statusCode != 404)
                Log.w(TAG, "get $url: status=${resp.statusCode} size=${resp.data.size}")
            return null
        }
        return String(resp.data, Charsets.UTF_8)
    }

    private fun parse(pnach: String, source: String): Pair<String, List<Entry>> {
        val gametitle = GAMETITLE_RE.find(pnach)?.groupValues?.get(1)?.trim().orEmpty()
        val entries = mutableListOf<Entry>()
        var name: String? = null
        var desc = ""
        val body = StringBuilder()
        fun flush() {
            val n = name
            if (n != null) entries.add(Entry(n, desc, body.toString().trimEnd(), source))
            name = null; desc = ""; body.setLength(0)
        }
        for (line in pnach.lines()) {
            val h = SECTION_RE.find(line)
            if (h != null) {
                flush()
                name = h.groupValues[1].trim()
                body.append(line).append('\n')
            } else if (name != null) {
                body.append(line).append('\n')
                if (desc.isEmpty()) COMMENT_RE.find(line)?.let { desc = it.groupValues[1].trim() }
            }
        }
        flush()
        return gametitle to entries
    }
}
