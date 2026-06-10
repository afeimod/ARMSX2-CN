package com.armsx2.ui

import android.content.Context
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.documentfile.provider.DocumentFile
import coil.compose.SubcomposeAsyncImage
import coil.request.ImageRequest
import com.armsx2.FilenameParser
import com.armsx2.GameInfo
import com.armsx2.GamePlatform
import com.armsx2.Main
import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONArray
import org.json.JSONObject

private val GAME_EXTENSIONS = setOf(
    "iso", "chd", "cso", "zso", "gz", "bin", "mdf", "img", "nrg", "dump"
)

object GamesList {
    /**
     * Cached list of games for the configured ROMs folder. Backed by
     * SharedPreferences (`gamesCache` JSON + `gamesCacheDir` URI) so a
     * cold start doesn't re-walk every file. The list is only refreshed
     * when:
     *   - the user changes their ROMs folder (auto-detected via the
     *     romsDir state — cached dir mismatch triggers a rescan), or
     *   - they explicitly tap the Refresh card in the grid.
     */
    private val games = mutableStateListOf<GameInfo>()
    private val scanning = mutableStateOf(false)
    private val scanError = mutableStateOf<String?>(null)
    private val lastScannedRoms = mutableStateOf<String?>(null)
    private val cacheLoaded = mutableStateOf(false)

    @Composable
    fun GamesRow() {
        val context = LocalContext.current
        val romsDirs = Main.romsDirs.value

        // Stable cache key — order-independent join of all configured dirs.
        // Two-dir configs in either order hit the same cache, single-dir
        // matches the old format. Used for both "is the cache stale?"
        // checks and the cache write below.
        val romsKey = remember(romsDirs) { cacheKeyForDirs(romsDirs) }

        LaunchedEffect(romsKey) {
            if (romsDirs.isEmpty()) return@LaunchedEffect

            if (!cacheLoaded.value) {
                cacheLoaded.value = true
                val (cachedKey, cachedGames) = loadCache(context)
                if (cachedKey == romsKey) {
                    games.clear()
                    games.addAll(cachedGames)
                    lastScannedRoms.value = romsKey
                    return@LaunchedEffect
                }
            }

            if (lastScannedRoms.value != romsKey && !scanning.value) {
                scanRoms(context, romsDirs, romsKey)
            }
        }

        Box(Modifier.fillMaxSize().padding(16.dp)) {
            when {
                romsDirs.isEmpty() -> EmptyMessage(
                    "No ROMs folders configured",
                    "Use the Settings cog to add one or more.",
                )
                scanning.value && games.isEmpty() -> ScanningSpinner()
                scanError.value != null -> Text(
                    scanError.value!!,
                    color = Color(0xFFFF6B6B),
                )
                else -> {
                    LazyVerticalGrid(
                        columns = GridCells.Fixed(5),
                        contentPadding = PaddingValues(4.dp),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalArrangement = Arrangement.spacedBy(8.dp),
                        modifier = Modifier.fillMaxSize(),
                    ) {
                        item(key = "__refresh__") {
                            BoxWithConstraints {
                                val gameCardHeight = maxWidth / 0.7f + 88.dp
                                Column(
                                    Modifier.height(gameCardHeight),
                                    verticalArrangement = Arrangement.spacedBy(8.dp),
                                ) {
                                    RefreshCard(
                                        modifier = Modifier.weight(1f),
                                        isScanning = scanning.value,
                                        onRefresh = {
                                            if (!scanning.value && romsDirs.isNotEmpty())
                                                scanRoms(context, romsDirs, romsKey)
                                        },
                                    )
                                    BiosCard(
                                        modifier = Modifier.weight(1f),
                                        onLaunch = {
                                            WindowImpl.showLibrary.value = false
                                            Main.startBios()
                                        },
                                    )
                                }
                            }
                        }
                        items(games, key = { it.uri.toString() }) { game ->
                            GameCard(game)
                        }
                    }
                }
            }
        }
    }

    /** Sorted "|" join so ["A","B"] and ["B","A"] produce the same key.
     *  We dedupe within the user's selection in case they accidentally
     *  pick the same folder twice. The "|v2" suffix invalidates legacy
     *  caches that were built before .img/.mdf/.nrg/.dump were probed for
     *  serials — bump again any time the probe coverage changes. */
    private fun cacheKeyForDirs(dirs: List<String>): String =
        dirs.toSet().sorted().joinToString("|") + "|v2"

    @Composable
    private fun EmptyMessage(title: String, body: String) {
        Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.Center) {
            Text(title, color = Color.White, fontSize = 18.sp,
                modifier = Modifier.align(Alignment.CenterHorizontally))
            Spacer(Modifier.height(8.dp))
            Text(body, color = Color.LightGray, fontSize = 14.sp,
                modifier = Modifier.align(Alignment.CenterHorizontally))
        }
    }

    @Composable
    private fun ScanningSpinner() {
        Row(Modifier.fillMaxSize(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.Center) {
            CircularProgressIndicator(
                modifier = Modifier.size(24.dp),
                strokeWidth = 2.dp,
                color = Colors.pasx2_blue,
            )
            Spacer(Modifier.width(12.dp))
            Text("Scanning ROMs…", color = Color.White)
        }
    }

    /** First grid slot — a refresh button styled like a game card.
     *  Caller passes Modifier.weight(1f) so this card and the BIOS card
     *  split the slot evenly. The icon Box uses weight(1f) too so the
     *  icon centers in whatever vertical space is left after the label. */
    @Composable
    private fun RefreshCard(
        modifier: Modifier = Modifier,
        isScanning: Boolean,
        onRefresh: () -> Unit,
    ) {
        Column(
            modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                // Already partial-alpha at 0.20, leave as-is; matches the
                // see-through aesthetic of the regular GameCard chrome.
                .background(Colors.pasx2_blue.copy(alpha = 0.20f))
                .border(2.dp, Colors.pasx2_blue.copy(alpha = 0.3f), RoundedCornerShape(8.dp))
                .clickable(enabled = !isScanning, onClick = onRefresh),
        ) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .weight(1f),
                contentAlignment = Alignment.Center,
            ) {
                if (isScanning) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(36.dp),
                        strokeWidth = 3.dp,
                        color = Color.White,
                    )
                } else {
                    Text("⟳", color = Color.White, fontSize = 56.sp)
                }
            }
            Column(Modifier.padding(8.dp)) {
                Text(
                    if (isScanning) "Scanning…" else "Refresh",
                    color = Color.White,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(2.dp))
                Text(
                    "Re-scan ROMs folder",
                    color = Color.LightGray,
                    fontSize = 11.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }

    /** Stacked under RefreshCard in the first grid slot — boots the
     *  PS2 BIOS without a disc, leaving the library/toolbar visible.
     *  The caller passes Modifier.weight(1f) so this card stretches to
     *  match the height of adjacent GameCards. The icon is centered in
     *  the flexible region; the label sits at the bottom. */
    @Composable
    private fun BiosCard(modifier: Modifier = Modifier, onLaunch: () -> Unit) {
        Column(
            modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                .background(Colors.pasx2_blue.copy(alpha = 0.20f))
                .border(2.dp, Colors.pasx2_blue.copy(alpha = 0.3f), RoundedCornerShape(8.dp))
                .clickable(onClick = onLaunch),
        ) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .weight(1f),
                contentAlignment = Alignment.Center,
            ) {
                Text("▶", color = Color.White, fontSize = 56.sp)
            }
            Column(Modifier.padding(8.dp)) {
                Text(
                    "Start BIOS",
                    color = Color.White,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.Bold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(2.dp))
                Text(
                    "Boot without a disc",
                    color = Color.LightGray,
                    fontSize = 11.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }

    @Composable
    private fun GameCard(game: GameInfo) {
        val context = LocalContext.current
        Column(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                // Card chrome (background + border) is partial-alpha so the
                // live game surface shows through when the library is
                // overlaid mid-play. The cover image painted into the inner
                // Box and the title/serial Text below stay fully opaque
                // because they're separate paint layers — alpha here only
                // affects the chrome, not the children.
                .background(Color(0xFF272525).copy(alpha = 0.3f))
                .border(1.dp, Color(0xFF3A3A3A).copy(alpha = 0.3f), RoundedCornerShape(8.dp))
                .clickable {
                    // Clear the library overlay (set by LoadGameButton while
                    // a game was running) so the surface for the new game
                    // comes up uncovered. No-op when starting from idle.
                    // Pass GameInfo so the in-game overlay has cover art /
                    // extension badge / pre-resolved compat stars ready.
                    WindowImpl.showLibrary.value = false
                    Main.launchGame(game.uri.toString(), game)
                },
        ) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .aspectRatio(0.7f) // PS2 cover boxart is taller than wide
                    .background(Color(0xFF1B1A1A).copy(alpha = 0.3f)),
                contentAlignment = Alignment.Center,
            ) {
                val coverUrl = game.coverUrl
                if (coverUrl != null) {
                    // SubcomposeAsyncImage so we can render a real fallback
                    // composable when the cover URL 404s — happens for any
                    // game the xlenore covers repo doesn't have (obscure
                    // releases, regional dumps whose serial isn't covered).
                    //
                    // Per-platform contentScale: PS2 boxart is taller than
                    // wide (matches the cell's 0.7 aspect ratio cleanly), so
                    // ContentScale.Crop fills the cell with no margin. PS1
                    // jewel-case covers are squarer/wider — cropping would
                    // chop the sides; ContentScale.Fit + Center letterboxes
                    // the cover inside the taller cell so the full art is
                    // visible. Cell aspect stays uniform so the grid layout
                    // doesn't shift between platforms.
                    val scale = when (game.platform) {
                        GamePlatform.PS2 -> ContentScale.Crop
                        GamePlatform.PS1 -> ContentScale.Fit
                    }
                    SubcomposeAsyncImage(
                        model = ImageRequest.Builder(context)
                            .data(coverUrl)
                            .crossfade(true)
                            .build(),
                        contentDescription = "${game.title} cover",
                        contentScale = scale,
                        alignment = Alignment.Center,
                        modifier = Modifier.fillMaxSize(),
                        loading = { CoverLoadingTile() },
                        error = { NoCoverTile(missingSerial = false) },
                    )
                } else {
                    NoCoverTile(missingSerial = true)
                }
            }
            Column(Modifier.padding(8.dp)) {
                Text(
                    game.title,
                    color = Color.White,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.Bold,
                    // Always reserve 2 lines so 1-line titles don't make
                    // their cards shorter than 2-line ones — keeps the
                    // grid rows uniform.
                    minLines = 2,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(2.dp))
                Text(
                    game.serial ?: "Unknown serial",
                    color = if (game.serial != null) Color(0xFFAACCFF) else Color(0xFF6F6F6F),
                    fontSize = 10.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(4.dp))
                Row(
                    Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    CompatibilityStars(game.compatibility)
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        if (game.extension.isNotEmpty()) ExtensionBadge(game.extension)
                        // Hardcore mode is a global RetroAchievements flag —
                        // when it's on, ANY game launched from here will
                        // apply, so render the HC badge on every card. The
                        // overlay's polling loop owns the flag.
                        if (InGameOverlay.hardcoreOn.value) {
                            Spacer(Modifier.width(4.dp))
                            HardcoreBadge()
                        }
                    }
                }
            }
        }
    }

    /** Small PS2-blue rounded chip showing the container format (ISO /
     *  CHD / BIN / etc.). Sits to the right of the compatibility stars. */
    @Composable
    private fun ExtensionBadge(ext: String) {
        Box(
            Modifier
                .clip(RoundedCornerShape(4.dp))
                .background(Colors.pasx2_blue)
                .padding(horizontal = 5.dp, vertical = 1.dp),
        ) {
            Text(
                ext,
                color = Color.White,
                fontSize = 9.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
            )
        }
    }

    /** RetroAchievements Hardcore badge. Shown on library cards while the
     *  global hardcore flag is set (so the user knows whichever game they
     *  launch will run in HC). Same shape as ExtensionBadge for visual
     *  consistency. */
    @Composable
    private fun HardcoreBadge() {
        Box(
            Modifier
                .clip(RoundedCornerShape(4.dp))
                .background(Color(0xFFB22222))
                .padding(horizontal = 5.dp, vertical = 1.dp),
        ) {
            Text(
                "HC",
                color = Color.White,
                fontSize = 9.sp,
                fontWeight = FontWeight.Bold,
                maxLines = 1,
            )
        }
    }

    /** Subtle spinner shown while the network fetch for a cover is in
     *  flight. Same dim background as the empty cover tile so the tile
     *  doesn't flash bright while loading. */
    @Composable
    private fun CoverLoadingTile() {
        Box(
            Modifier.fillMaxSize().background(Color(0xFF1B1A1A).copy(alpha = 0.3f)),
            contentAlignment = Alignment.Center,
        ) {
            CircularProgressIndicator(
                modifier = Modifier.size(20.dp),
                strokeWidth = 2.dp,
                color = Color(0xFF555555),
            )
        }
    }

    /** Fallback cover tile rendered when:
     *    - missingSerial = true  (we couldn't extract a serial at all
     *                             → no URL to even try) → "?" in red-ish tint
     *    - missingSerial = false (URL was tried and 404'd / network err)
     *                            → disc icon + "No cover" subtitle
     *  In both cases the title + serial are still visible below the tile.
     */
    @Composable
    private fun NoCoverTile(missingSerial: Boolean) {
        Box(
            Modifier.fillMaxSize().background(Color(0xFF1B1A1A).copy(alpha = 0.3f)),
            contentAlignment = Alignment.Center,
        ) {
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                if (missingSerial) {
                    Text("?", color = Color(0xFF8A4A4A), fontSize = 56.sp)
                    Text("No serial", color = Color(0xFF8A4A4A), fontSize = 10.sp)
                } else {
                    Text("📀", color = Color(0xFF3F3F3F), fontSize = 56.sp)
                    Text("No cover", color = Color(0xFF6F6F6F), fontSize = 10.sp)
                }
            }
        }
    }

    @Composable
    private fun CompatibilityStars(filled: Int) {
        Row {
            repeat(5) { i ->
                val on = i < filled
                Text(
                    if (on) "★" else "☆",
                    color = if (on) Color(0xFFFFD33A) else Color(0xFF555555),
                    fontSize = 12.sp,
                )
            }
        }
    }

    /**
     * Walk every configured ROM folder via DocumentFile, probe each disc
     * image's SYSTEM.CNF for its real serial + platform when possible,
     * and persist merged results to prefs. Falls back to filename-based
     * serial extraction for compressed formats (CHD/CSO/GZ) or non-ISO9660
     * raw images.
     *
     * De-duplication: same URI in two configured folders (rare — would
     * require nested SAF mounts) is merged by URI string. Title sort is
     * case-insensitive across the union.
     */
    private fun scanRoms(context: Context, romsUriStrings: List<String>, romsKey: String) {
        scanning.value = true
        scanError.value = null
        lastScannedRoms.value = romsKey
        Main.invoke {
            try {
                val collected = linkedMapOf<String, GameInfo>() // URI → info, preserves first-seen order
                for (dirUri in romsUriStrings) {
                    val uri = try { Uri.parse(dirUri) } catch (_: Exception) { null } ?: continue
                    val tree = DocumentFile.fromTreeUri(context, uri) ?: continue
                    val files = tree.listFiles()
                    for (f in files) {
                        if (!f.isFile) continue
                        val name = f.name ?: continue
                        val ext = name.substringAfterLast('.', "").lowercase()
                        if (ext !in GAME_EXTENSIONS) continue

                        // ISO9660 probe (PS2 BOOT2 + PS1 BOOT). Native
                        // returns "<platform>:<serial>" tag — e.g.
                        // "ps1:SLUS-00713" — when SYSTEM.CNF is parseable.
                        // Compressed formats (cso/zso/gz) fall through to
                        // filename. Raw-sector formats (.img/.mdf/.nrg/
                        // .dump) are handled by the same plain-ISO path in
                        // native — they fall through to the 2352/16 and
                        // 2352/24 fallbacks if 2048/0 fails.
                        val rawProbe = when (ext) {
                            "iso", "bin", "chd", "img", "mdf", "nrg", "dump" ->
                                probeDiscSerial(context, f.uri)
                            else -> null
                        }
                        val (probeSerial, probePlatform) = parseProbeResult(rawProbe)
                        val (titleFromName, serialFromName) = FilenameParser.parse(name)
                        val finalSerial = probeSerial ?: serialFromName
                        val finalPlatform = probePlatform ?: GamePlatform.PS2
                        // PCSX2 gamedb returns 0..6 (Unknown / Nothing /
                        // Intro / Menu / InGame / Playable / Perfect).
                        // Map 0,1 → 0 stars; 2..6 → 1..5 stars. PS1 will
                        // typically miss the PS2-only gamedb and stay 0.
                        val compatRaw = if (finalSerial != null)
                            NativeApp.getCompatibilityForSerial(finalSerial) else 0
                        val compatStars = (compatRaw - 1).coerceIn(0, 5)
                        val info = GameInfo(
                            uri = f.uri,
                            title = titleFromName,
                            serial = finalSerial,
                            compatibility = compatStars,
                            extension = ext.uppercase(),
                            platform = finalPlatform,
                        )
                        collected.putIfAbsent(f.uri.toString(), info)
                    }
                }
                val sorted = collected.values.sortedBy { it.title.lowercase() }
                games.clear()
                games.addAll(sorted)
                saveCache(context, romsKey, sorted)
            } catch (e: Exception) {
                scanError.value = "Scan failed: ${e.message}"
            } finally {
                scanning.value = false
            }
        }
    }

    /** Split a "ps1:SLUS-00713" / "ps2:SLUS-20312" probe return into
     *  (serial, platform). Untagged input (legacy native or filename-only)
     *  falls back to (input, null). null/empty → (null, null). */
    private fun parseProbeResult(raw: String?): Pair<String?, GamePlatform?> {
        if (raw.isNullOrEmpty()) return null to null
        val colon = raw.indexOf(':')
        if (colon <= 0) return raw to null
        val tag = raw.substring(0, colon)
        val serial = raw.substring(colon + 1)
        return serial to GamePlatform.fromKey(tag)
    }

    /** Open `uri` for read, hand the fd to native, get the
     *  "<platform>:<serial>" back. */
    private fun probeDiscSerial(context: Context, uri: Uri): String? {
        return try {
            val pfd = context.contentResolver.openFileDescriptor(uri, "r") ?: return null
            val fd = pfd.detachFd()
            NativeApp.getGameSerialFromFd(fd) // consumes fd
        } catch (_: Exception) {
            null
        }
    }

    // ---------------- Prefs cache ----------------

    private fun saveCache(context: Context, romsKey: String, list: List<GameInfo>) {
        val arr = JSONArray()
        for (g in list) {
            arr.put(JSONObject().apply {
                put("uri", g.uri.toString())
                put("title", g.title)
                put("serial", g.serial ?: JSONObject.NULL)
                put("compat", g.compatibility)
                put("ext", g.extension)
                put("platform", g.platform.key)
            })
        }
        Main.prefs.edit()
            .putString("gamesCacheKey", romsKey)
            .putString("gamesCache", arr.toString())
            .apply()
    }

    private fun loadCache(context: Context): Pair<String?, List<GameInfo>> {
        val prefs = Main.prefs
        // New key format ("|"-joined sorted dir set) under "gamesCacheKey".
        // Legacy key (single dir) was under "gamesCacheDir" — read either,
        // prefer the new one. Caller compares against the current
        // cacheKeyForDirs() so a legacy single-dir cache still matches an
        // unchanged single-dir config.
        val cachedKey = prefs.getString("gamesCacheKey", null)
            ?: prefs.getString("gamesCacheDir", null)
        val cachedJson = prefs.getString("gamesCache", null) ?: return cachedKey to emptyList()
        return try {
            val arr = JSONArray(cachedJson)
            val list = mutableListOf<GameInfo>()
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                val rawSerial = if (obj.isNull("serial")) null else obj.optString("serial").takeIf { it.isNotEmpty() }
                list.add(GameInfo(
                    uri = Uri.parse(obj.getString("uri")),
                    title = obj.getString("title"),
                    serial = rawSerial,
                    compatibility = obj.optInt("compat", 0),
                    extension = obj.optString("ext", "").ifEmpty {
                        obj.getString("uri").substringAfterLast('.', "").uppercase()
                    },
                    platform = GamePlatform.fromKey(obj.optString("platform", null)),
                ))
            }
            cachedKey to list
        } catch (_: Exception) {
            cachedKey to emptyList()
        }
    }
}
