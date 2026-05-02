package com.armsx2.ui

import android.content.Context
import android.net.Uri
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
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
        val romsDir = Main.romsDir.value

        // Cache-or-scan: hydrate from prefs on first composition, then
        // scan only when the configured dir differs from what's cached.
        // Sequential inside one LaunchedEffect so there's no race between
        // the hydration and the auto-scan.
        LaunchedEffect(romsDir) {
            if (romsDir == null) return@LaunchedEffect

            if (!cacheLoaded.value) {
                cacheLoaded.value = true
                val (cachedDir, cachedGames) = loadCache(context)
                if (cachedDir == romsDir) {
                    games.clear()
                    games.addAll(cachedGames)
                    lastScannedRoms.value = romsDir
                    return@LaunchedEffect
                }
            }

            if (lastScannedRoms.value != romsDir && !scanning.value) {
                scanRoms(context, romsDir)
            }
        }

        Box(Modifier.fillMaxSize().padding(16.dp)) {
            when {
                romsDir == null -> EmptyMessage(
                    "No ROMs folder configured",
                    "Use the Settings cog to set one up.",
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
                            RefreshCard(
                                isScanning = scanning.value,
                                onRefresh = {
                                    if (!scanning.value && romsDir != null)
                                        scanRoms(context, romsDir)
                                },
                            )
                        }
                        items(games, key = { it.uri.toString() }) { game ->
                            GameCard(game)
                        }
                    }
                }
            }
        }
    }

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

    /** First grid slot — a refresh button styled like a game card. */
    @Composable
    private fun RefreshCard(isScanning: Boolean, onRefresh: () -> Unit) {
        Column(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                .background(Colors.pasx2_blue.copy(alpha = 0.20f))
                .border(2.dp, Colors.pasx2_blue, RoundedCornerShape(8.dp))
                .clickable(enabled = !isScanning, onClick = onRefresh),
        ) {
            // Square hero box mirrors the GameCard cover area so all grid
            // slots line up nicely. PS2 covers are roughly 4:3 (1.33) but
            // we use 1:1 here to keep the refresh icon centered.
            Box(
                Modifier
                    .fillMaxWidth()
                    .aspectRatio(1f),
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

    @Composable
    private fun GameCard(game: GameInfo) {
        val context = LocalContext.current
        Column(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(8.dp))
                .background(Color(0xFF272525))
                .border(1.dp, Color(0xFF3A3A3A), RoundedCornerShape(8.dp))
                .clickable {
                    // TODO: hook into VMManager.runVMThread once SAF-aware
                    // load is wired (LoadGameButton equivalent for URI).
                },
        ) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .aspectRatio(0.7f) // PS2 cover boxart is taller than wide
                    .background(Color(0xFF1B1A1A)),
                contentAlignment = Alignment.Center,
            ) {
                val coverUrl = game.coverUrl
                if (coverUrl != null) {
                    // SubcomposeAsyncImage so we can render a real fallback
                    // composable when the cover URL 404s — happens for any
                    // game the xlenore/ps2-covers `default/` folder doesn't
                    // have (e.g. obscure releases, regional dumps whose
                    // serial isn't covered there).
                    SubcomposeAsyncImage(
                        model = ImageRequest.Builder(context)
                            .data(coverUrl)
                            .crossfade(true)
                            .build(),
                        contentDescription = "${game.title} cover",
                        contentScale = ContentScale.Crop,
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
                CompatibilityStars(game.compatibility)
            }
        }
    }

    /** Subtle spinner shown while the network fetch for a cover is in
     *  flight. Same dim background as the empty cover tile so the tile
     *  doesn't flash bright while loading. */
    @Composable
    private fun CoverLoadingTile() {
        Box(
            Modifier.fillMaxSize().background(Color(0xFF1B1A1A)),
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
            Modifier.fillMaxSize().background(Color(0xFF1B1A1A)),
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
     * Walk the ROMs folder via DocumentFile, probe each disc image's
     * SYSTEM.CNF for its real serial when possible, and persist results
     * to prefs. Falls back to filename-based serial extraction for any
     * file the C++ probe rejects (CHD / CSO / GZ / etc., or ISOs whose
     * filesystem doesn't follow the typical PS2 layout).
     */
    private fun scanRoms(context: Context, romsUriString: String) {
        scanning.value = true
        scanError.value = null
        lastScannedRoms.value = romsUriString
        Main.invoke {
            try {
                val uri = Uri.parse(romsUriString)
                val tree = DocumentFile.fromTreeUri(context, uri)
                val files = tree?.listFiles() ?: emptyArray()
                val collected = mutableListOf<GameInfo>()
                for (f in files) {
                    if (!f.isFile) continue
                    val name = f.name ?: continue
                    val ext = name.substringAfterLast('.', "").lowercase()
                    if (ext !in GAME_EXTENSIONS) continue

                    // Try the real ISO9660 probe first. Native handles
                    // 2048/0 (.iso) and 2352/{16,24} (.bin Mode 1/Mode 2
                    // raw) layouts, so both formats can yield real serials.
                    // CHD/CSO/GZ/etc. need a decompressor — not yet wired
                    // — so they fall through to filename parsing.
                    val realSerial = if (ext == "iso" || ext == "bin")
                        probeDiscSerial(context, f.uri) else null
                    val (titleFromName, serialFromName) = FilenameParser.parse(name)
                    val finalSerial = realSerial ?: serialFromName
                    // PCSX2 gamedb returns 0..6 (Unknown / Nothing / Intro /
                    // Menu / InGame / Playable / Perfect). Per spec we treat
                    // 0 and 1 as zero stars (no usable progress) and 2..6
                    // map to 1..5 stars.
                    val compatRaw = if (finalSerial != null)
                        NativeApp.getCompatibilityForSerial(finalSerial) else 0
                    val compatStars = (compatRaw - 1).coerceIn(0, 5)
                    collected += GameInfo(
                        uri = f.uri,
                        title = titleFromName,
                        serial = finalSerial,
                        compatibility = compatStars,
                    )
                }
                collected.sortBy { it.title.lowercase() }
                games.clear()
                games.addAll(collected)
                saveCache(context, romsUriString, collected)
            } catch (e: Exception) {
                scanError.value = "Scan failed: ${e.message}"
            } finally {
                scanning.value = false
            }
        }
    }

    /** Open `uri` for read, hand the fd to native, get the serial back. */
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

    private fun saveCache(context: Context, romsUri: String, list: List<GameInfo>) {
        val arr = JSONArray()
        for (g in list) {
            arr.put(JSONObject().apply {
                put("uri", g.uri.toString())
                put("title", g.title)
                put("serial", g.serial ?: JSONObject.NULL)
                put("compat", g.compatibility)
            })
        }
        Main.prefs.edit()
            .putString("gamesCacheDir", romsUri)
            .putString("gamesCache", arr.toString())
            .apply()
    }

    private fun loadCache(context: Context): Pair<String?, List<GameInfo>> {
        val prefs = Main.prefs
        val cachedDir = prefs.getString("gamesCacheDir", null)
        val cachedJson = prefs.getString("gamesCache", null) ?: return cachedDir to emptyList()
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
                ))
            }
            cachedDir to list
        } catch (_: Exception) {
            cachedDir to emptyList()
        }
    }
}
