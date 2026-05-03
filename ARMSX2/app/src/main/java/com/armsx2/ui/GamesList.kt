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
                            // LazyVerticalGrid hands items unbounded height,
                            // so fillMaxHeight() inside an item collapses to
                            // zero — siblings can't be matched implicitly.
                            // Use BoxWithConstraints to read the cell width,
                            // then size this slot to the same intrinsic
                            // height a GameCard would have at that width:
                            //   hero box (aspect 0.7 → width / 0.7) +
                            //   ~64dp label/serial/stars column.
                            BoxWithConstraints {
                                // Hero (aspect 0.7 → width/0.7) + label
                                // column. With minLines=2 on the title the
                                // label is now always:
                                //   8dp pad + 2×13sp title (~36dp) +
                                //   2dp + 10sp serial (~14dp) +
                                //   4dp + 12sp stars (~16dp) + 8dp pad
                                //   ≈ 88dp.
                                val gameCardHeight = maxWidth / 0.7f + 88.dp
                                Column(
                                    Modifier.height(gameCardHeight),
                                    verticalArrangement = Arrangement.spacedBy(8.dp),
                                ) {
                                    RefreshCard(
                                        modifier = Modifier.weight(1f),
                                        isScanning = scanning.value,
                                        onRefresh = {
                                            if (!scanning.value && romsDir != null)
                                                scanRoms(context, romsDir)
                                        },
                                    )
                                    BiosCard(
                                        modifier = Modifier.weight(1f),
                                        onLaunch = {
                                            // Force the library overlay on so the
                                            // cards remain visible once eState flips
                                            // to RUNNING — the STOPPED-branch list
                                            // disappears at that point and WindowImpl
                                            // re-paints it via the showLibrary path.
                                            WindowImpl.showLibrary.value = true
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
                    if (game.extension.isNotEmpty()) ExtensionBadge(game.extension)
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
                    // 2048/0 (.iso), 2352/{16,24} (.bin Mode 1/Mode 2 raw),
                    // and CHD via libchdr (DVD + CD frame sizes). Other
                    // compressed formats (CSO/GZ/etc.) still fall through
                    // to filename parsing.
                    val realSerial = if (ext == "iso" || ext == "bin" || ext == "chd")
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
                        extension = ext.uppercase(),
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
                put("ext", g.extension)
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
                    // Older caches won't have "ext" — fall back to deriving
                    // it from the URI path so existing entries still get a
                    // badge without forcing a refresh.
                    extension = obj.optString("ext", "").ifEmpty {
                        obj.getString("uri").substringAfterLast('.', "").uppercase()
                    },
                ))
            }
            cachedDir to list
        } catch (_: Exception) {
            cachedDir to emptyList()
        }
    }
}
