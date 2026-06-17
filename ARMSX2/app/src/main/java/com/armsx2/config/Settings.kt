package com.armsx2.config

import kr.co.iefriends.pcsx2.NativeApp
import org.json.JSONObject

/**
 * Resolved emulator config used to drive a VM launch / live-apply.
 *
 * Field naming convention: each field comments the upstream
 * `<section>/<key>` it maps to (grep against pcsx2 docs / Pcsx2Config.cpp).
 *
 * Settings are pushed via [applyTo] which calls NativeApp.setSetting per
 * field, then a single NativeApp.commitSettings to push the queued writes
 * into the running VM (or persist them for the next launch).
 *
 * Adding a new setting:
 *   1. Add a field with an upstream-matching default,
 *   2. Add a setSetting line in applyTo,
 *   3. Add the JSON mapping in toJson + fromJson + merge,
 *   4. Surface a widget in the appropriate Settings tab.
 */
data class Settings(
    // ---- EmuCore/Speedhacks ----
    /** EmuCore/Speedhacks/EECycleRate — −3..+3 (50%..300%). 0 = nominal. */
    val eeCycleRate: Int = 0,
    /** EmuCore/Speedhacks/EECycleSkip — 0..3. 0 = no skip. */
    val eeCycleSkip: Int = 0,
    /** EmuCore/Speedhacks/vuThread — Multi-Threaded VU1 (MTVU).
     *  Kept on by default for the mac ARM64 backend. Old Android Refresh
     *  config blobs may contain mtvu=false, so loading/merging ignores stale
     *  persisted values while the live toggle can still A/B a running game. */
    val mtvu: Boolean = true,
    /** EmuCore/Speedhacks/vu1Instant — completes VU1 in one cycle. */
    val vu1Instant: Boolean = true,
    /** EmuCore/Speedhacks/vuFlagHack — skip VU flag computation when unread. */
    val vuFlagHack: Boolean = true,
    /** EmuCore/Speedhacks/fastCDVD — skip CDVD reads. */
    val fastCDVD: Boolean = false,
    /** EmuCore/Speedhacks/IntcStat — INTC_STAT register read hack. */
    val intcStat: Boolean = true,
    /** EmuCore/Speedhacks/WaitLoop — detect EE wait loops. */
    val waitLoop: Boolean = true,
    /** EmuCore/Speedhacks/vuNeonFusions — ARMSX2-only. Gates the arm64
     *  VU1 JIT NEON peephole fusions (MAC cluster
     *  MULAx+MADDAy+MADDAz+MADDw, OPMULA+OPMSUB cross-product). Default
     *  on — toggle off to A/B whether one of those JIT fusions is
     *  responsible for a per-game regression. */
    val vuNeonFusions: Boolean = true,
    /** EmuCore/Speedhacks/vuDeferredWrites — EXPERIMENTAL. Defers
     *  per-pair VF stores via the NEON cache; flush sites commit later.
     *  Big perf win on transform-heavy code. Known to break SH2 graphics
     *  and other games with cross-pair memory coherence assumptions. */
    val vuDeferredWrites: Boolean = false,
    /** EmuCore/Speedhacks/vuSkipStallSim — AGGRESSIVE. Skips the
     *  vu1_TestPipes_VU1 BL in the JIT — was 19-32% of total CPU on
     *  Futurama/GoW2/Ape Escape 3 per profiling. Breaks any game that
     *  relies on accurate FMAC/FDIV/EFU/IALU pipeline-stall timing. */
    val vuSkipStallSim: Boolean = false,

    // ---- EmuCore/GS — frame limiter ----
    /** EmuCore/GS/FrameLimitEnable. */
    val frameLimitEnable: Boolean = true,
    /** EmuCore/GS/AspectRatio:
     *  0 Stretch · 1 Auto 4:3/3:2 · 2 4:3 · 3 16:9 · 4 10:7. */
    val aspectRatio: Int = 1,

    // ---- DEV9 — PS2 HDD / Ethernet ----
    /** DEV9/Eth/EthEnable — PS2 network adapter. */
    val dev9EthEnable: Boolean = false,
    /** DEV9/Eth/EthApi — "Sockets" is the usable Android backend. */
    val dev9EthApi: String = "Sockets",
    /** DEV9/Eth/EthDevice — "Auto" lets the sockets backend choose. */
    val dev9EthDevice: String = "Auto",
    /** DEV9/Hdd/HddEnable — virtual PS2 HDD. */
    val dev9HddEnable: Boolean = false,
    /** DEV9/Hdd/HddFile — path/name of the virtual HDD image. */
    val dev9HddFile: String = "DEV9hdd.raw",

    // ---- EmuCore/CPU/Recompiler — recompiler enables ----
    /** EmuCore/CPU/Recompiler/EnableEE — EE (R5900) recompiler. */
    val recEE: Boolean = true,
    /** EmuCore/CPU/Recompiler/EnableIOP — IOP (R3000) recompiler. */
    val recIOP: Boolean = true,
    /** EmuCore/CPU/Recompiler/EnableVU0 — VU0 recompiler. */
    val recVU0: Boolean = true,
    /** EmuCore/CPU/Recompiler/EnableVU1 — VU1 recompiler. */
    val recVU1: Boolean = true,
    /** EmuCore/CPU/Recompiler/EnableFastmem — fastmem (page-fault backpatch
     *  signal handler). Disabling falls back to the slow VTLB read/write
     *  path on every memory op. */
    val enableFastmem: Boolean = true,

    // ---- macOS/PCSX2 ARM64 backend compatibility flags ----
    // Hidden from UI and forced on. Kept only so older JSON/INI/per-game blobs
    // with UseMac* keys still parse without losing the rest of their settings.
    /** EmuCore/CPU/Recompiler/UseMacEE — legacy, forced on. */
    val useMacEE: Boolean = true,
    /** EmuCore/CPU/Recompiler/UseMacIOP — legacy, forced on. */
    val useMacIOP: Boolean = true,
    /** EmuCore/CPU/Recompiler/UseMacVU0 — legacy, forced on. */
    val useMacVU0: Boolean = true,
    /** EmuCore/CPU/Recompiler/UseMacVU1 — legacy, forced on. */
    val useMacVU1: Boolean = true,

    // ---- microVU-style compile-time pipeline-stall folding ----
    /** EmuCore/CPU/Recompiler/Vu1InlineFmacStall — replace the per-pair
     *  `vu1_TestFMACStallReg / _Reg2` BLs (formerly 17-32% of total CPU per
     *  simpleperf) with an inline `Add VU1_CYCLE_REG, #fmac_stall`. Mirrors
     *  mac's compile-time mVUincCycles + mVUstall fold. Gated by the same
     *  `fmac_carry_safe` (ct_cycle > 3) guarantee that cross-block carry-in
     *  FMAC slots have retired at runtime. */
    val vu1InlineFmacStall: Boolean = false,
    /** EmuCore/CPU/Recompiler/Vu1CrossBlockPState — propagate predecessor's
     *  exit pipeline-state to successor block compile, so CARRY_IN_GATE_*
     *  bounds can shrink (FMAC/IALU=3, FDIV=12, EFU=54). When a predecessor
     *  links to a successor, the successor variant is specialised for that
     *  predecessor's exitState. Mirrors mac's microBlockManager pState match. */
    val vu1CrossBlockPState: Boolean = false,
    /** EmuCore/CPU/Recompiler/Vu1InlineDrainTestPipes — inline-emit the
     *  vu1_TestPipes_VU1 FMAC drain at JIT sites where the pre-walk proves
     *  FDIV/EFU/IALU are empty (skip_info[i].fmacOnlyTestPipes). Saves the BL
     *  + viCacheInvalidateAll + return overhead per call. Mac doesn't need
     *  this because it has no runtime FMAC ring — flag instances are routed
     *  at compile time. */
    val vu1InlineDrainTestPipes: Boolean = false,
    /** EmuCore/CPU/Recompiler/Vu1FmacInstanceRouting — mac-style 4-slot flag-
     *  instance routing. Repurposes VU->fmac[0..3].{mac,status,clip}flag as
     *  instance slots; skips the ring metadata Strs and the FMAC stall BLs.
     *  fmaccount stays 0 so vu1_TestPipes_VU1's FMAC drain early-exits. */
    val vu1FmacInstanceRouting: Boolean = false,

    // ---- EmuCore/GS — renderer accuracy / quality ----
    /** EmuCore/GS/hw_mipmap. */
    val hwMipmap: Boolean = true,
    /** EmuCore/GS/accurate_blending_unit — AccBlendLevel:
     *  0 Min · 1 Basic · 2 Medium · 3 High · 4 Full · 5 Maximum. */
    val accurateBlendingUnit: Int = 4,
    /** EmuCore/GS/filter — BiFiltering:
     *  0 Nearest · 1 Forced (Bilinear) · 2 PS2 · 3 Forced_But_Sprite. */
    val textureFiltering: Int = 2,
    /** EmuCore/GS/texture_preloading — TexturePreloadingLevel:
     *  0 Off · 1 Partial · 2 Full. */
    val texturePreloading: Int = 2,
    /** EmuCore/GS/LoadTextureReplacements. */
    val loadTextureReplacements: Boolean = false,
    /** EmuCore/GS/LoadTextureReplacementsAsync. */
    val loadTextureReplacementsAsync: Boolean = true,
    /** EmuCore/GS/PrecacheTextureReplacements. */
    val precacheTextureReplacements: Boolean = false,
    /** EmuCore/GS/DumpReplaceableTextures. */
    val dumpReplaceableTextures: Boolean = false,
    /** EmuCore/GS/OsdShowTextureReplacements. */
    val osdShowTextureReplacements: Boolean = false,
    /** EmuCore/GS/UserHacks_AutoFlushLevel — GSHWAutoFlushLevel:
     *  0 Disabled · 1 SpritesOnly · 2 Enabled. */
    val autoFlush: Int = 0,
    /** EmuCore/GS/UserHacks_HalfPixelOffset — GSHalfPixelOffset:
     *  0 Off · 1 Normal · 2 Special · 3 SpecialAggressive · 4 Native · 5 NativeWTexOffset. */
    val halfPixelOffset: Int = 0,
    /** EmuCore/GS/TriFilter — TriFiltering: -1 Auto · 0 Off · 1 PS2 · 2 Forced. */
    val triFilter: Int = -1,
    /** EmuCore/GS/MaxAnisotropy — 0 Off, else 2/4/8/16. */
    val maxAnisotropy: Int = 0,
    /** EmuCore/GS/AndroidGpuProfileOverride — 0 Auto · 1 Mali · 2 Adreno · 3 PowerVR.
     *  Stringified to "auto"/"mali"/"adreno"/"powervr" when written to emucore.
     *  Picked up in GSDeviceOGL::CheckFeatures at device init; requires
     *  a renderer restart to take effect. */
    val gpuProfile: Int = 0,
) {
    /** Push every field into emucore via NativeApp.setSetting + commit. */
    fun applyTo() {
        // Speedhacks
        NativeApp.setSetting("EmuCore/Speedhacks", "EECycleRate", "int", eeCycleRate.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "EECycleSkip", "int", eeCycleSkip.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "vuThread", "bool", mtvu.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "vu1Instant", "bool", vu1Instant.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "vuFlagHack", "bool", vuFlagHack.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "fastCDVD", "bool", fastCDVD.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "IntcStat", "bool", intcStat.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "WaitLoop", "bool", waitLoop.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "vuNeonFusions", "bool", vuNeonFusions.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "vuDeferredWrites", "bool", vuDeferredWrites.toString())
        NativeApp.setSetting("EmuCore/Speedhacks", "vuSkipStallSim", "bool", vuSkipStallSim.toString())
        // GS frame limit. The setting key is persisted (read by runVMThread
        // after Initialize so cold starts honor the preference) AND the live
        // limiter mode is poked via speedhackLimitermode so toggling in-game
        // takes effect immediately. 0 = Nominal (capped at native rate),
        // 3 = Unlimited.
        NativeApp.setSetting("EmuCore/GS", "FrameLimitEnable", "bool", frameLimitEnable.toString())
        NativeApp.speedhackLimitermode(if (frameLimitEnable) 0 else 3)
        val aspectRatioName = when (aspectRatio.coerceIn(0, 4)) {
            0 -> "Stretch"
            2 -> "4:3"
            3 -> "16:9"
            4 -> "10:7"
            else -> "Auto 4:3/3:2"
        }
        NativeApp.setSetting("EmuCore/GS", "AspectRatio", "string", aspectRatioName)
        NativeApp.setAspectRatio(aspectRatio.coerceIn(0, 4))
        // DEV9. Networking/HDD are initialized with the VM, so changes
        // made from the in-game overlay are persisted for the next boot.
        NativeApp.setSetting("DEV9/Eth", "EthEnable", "bool", dev9EthEnable.toString())
        NativeApp.setSetting("DEV9/Eth", "EthApi", "string", dev9EthApi)
        NativeApp.setSetting("DEV9/Eth", "EthDevice", "string", dev9EthDevice.ifEmpty { "Auto" })
        NativeApp.setSetting("DEV9/Hdd", "HddEnable", "bool", dev9HddEnable.toString())
        NativeApp.setSetting("DEV9/Hdd", "HddFile", "string", dev9HddFile.ifEmpty { "DEV9hdd.raw" })
        // Recompiler enables. Picked up by VMManager::ApplySettings →
        // SysCpuProviderPack rebind. Toggling these on a running VM swaps
        // the dispatch pointer; existing JIT block caches are flushed by
        // ApplySettings's CpusChanged path.
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "EnableEE", "bool", recEE.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "EnableIOP", "bool", recIOP.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "EnableVU0", "bool", recVU0.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "EnableVU1", "bool", recVU1.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "EnableFastmem", "bool", enableFastmem.toString())
        // Force the single macOS/PCSX2 ARM64 backend. VMManager also ignores
        // stale UseMac* values, but writing true cleans old persisted settings.
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "UseMacEE", "bool", "true")
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "UseMacIOP", "bool", "true")
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "UseMacVU0", "bool", "true")
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "UseMacVU1", "bool", "true")
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "Vu1InlineFmacStall", "bool", vu1InlineFmacStall.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "Vu1CrossBlockPState", "bool", vu1CrossBlockPState.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "Vu1InlineDrainTestPipes", "bool", vu1InlineDrainTestPipes.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "Vu1FmacInstanceRouting", "bool", vu1FmacInstanceRouting.toString())
        // GS renderer
        NativeApp.setSetting("EmuCore/GS", "hw_mipmap", "bool", hwMipmap.toString())
        NativeApp.setSetting("EmuCore/GS", "accurate_blending_unit", "int", accurateBlendingUnit.toString())
        NativeApp.setSetting("EmuCore/GS", "filter", "int", textureFiltering.toString())
        NativeApp.setSetting("EmuCore/GS", "texture_preloading", "int", texturePreloading.toString())
        NativeApp.setSetting("EmuCore/GS", "LoadTextureReplacements", "bool", loadTextureReplacements.toString())
        NativeApp.setSetting("EmuCore/GS", "LoadTextureReplacementsAsync", "bool", loadTextureReplacementsAsync.toString())
        NativeApp.setSetting("EmuCore/GS", "PrecacheTextureReplacements", "bool", precacheTextureReplacements.toString())
        NativeApp.setSetting("EmuCore/GS", "DumpReplaceableTextures", "bool", dumpReplaceableTextures.toString())
        NativeApp.setSetting("EmuCore/GS", "OsdShowTextureReplacements", "bool", osdShowTextureReplacements.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_AutoFlushLevel", "int", autoFlush.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_HalfPixelOffset", "int", halfPixelOffset.toString())
        NativeApp.setSetting("EmuCore/GS", "TriFilter", "int", triFilter.toString())
        NativeApp.setSetting("EmuCore/GS", "MaxAnisotropy", "int", maxAnisotropy.toString())
        val gpuProfileStr = when (gpuProfile) {
            1 -> "mali"
            2 -> "adreno"
            3 -> "powervr"
            else -> "auto"
        }
        NativeApp.setSetting("EmuCore/GS", "AndroidGpuProfileOverride", "string", gpuProfileStr)
        NativeApp.commitSettings()
    }

    fun toJson(): JSONObject = JSONObject().apply {
        put("eeCycleRate", eeCycleRate)
        put("eeCycleSkip", eeCycleSkip)
        put("mtvu", true)
        put("vu1Instant", vu1Instant)
        put("vuFlagHack", vuFlagHack)
        put("fastCDVD", fastCDVD)
        put("intcStat", intcStat)
        put("waitLoop", waitLoop)
        put("vuNeonFusions", vuNeonFusions)
        put("vuDeferredWrites", vuDeferredWrites)
        put("vuSkipStallSim", vuSkipStallSim)
        put("frameLimitEnable", frameLimitEnable)
        put("aspectRatio", aspectRatio)
        put("dev9EthEnable", dev9EthEnable)
        put("dev9EthApi", dev9EthApi)
        put("dev9EthDevice", dev9EthDevice)
        put("dev9HddEnable", dev9HddEnable)
        put("dev9HddFile", dev9HddFile)
        put("recEE", recEE)
        put("recIOP", recIOP)
        put("recVU0", recVU0)
        put("recVU1", recVU1)
        put("enableFastmem", enableFastmem)
        put("vu1InlineFmacStall", vu1InlineFmacStall)
        put("vu1CrossBlockPState", vu1CrossBlockPState)
        put("vu1InlineDrainTestPipes", vu1InlineDrainTestPipes)
        put("vu1FmacInstanceRouting", vu1FmacInstanceRouting)
        put("hwMipmap", hwMipmap)
        put("accurateBlendingUnit", accurateBlendingUnit)
        put("textureFiltering", textureFiltering)
        put("texturePreloading", texturePreloading)
        put("loadTextureReplacements", loadTextureReplacements)
        put("loadTextureReplacementsAsync", loadTextureReplacementsAsync)
        put("precacheTextureReplacements", precacheTextureReplacements)
        put("dumpReplaceableTextures", dumpReplaceableTextures)
        put("osdShowTextureReplacements", osdShowTextureReplacements)
        put("autoFlush", autoFlush)
        put("halfPixelOffset", halfPixelOffset)
        put("triFilter", triFilter)
        put("maxAnisotropy", maxAnisotropy)
        put("gpuProfile", gpuProfile)
    }

    companion object {
        /** Lenient parse — missing keys fall back to defaults so old saved
         *  blobs survive when new fields are added. */
        fun fromJson(json: JSONObject): Settings {
            val def = Settings()
            return Settings(
                eeCycleRate = json.optInt("eeCycleRate", def.eeCycleRate),
                eeCycleSkip = json.optInt("eeCycleSkip", def.eeCycleSkip),
                mtvu = true,
                vu1Instant = json.optBoolean("vu1Instant", def.vu1Instant),
                vuFlagHack = json.optBoolean("vuFlagHack", def.vuFlagHack),
                fastCDVD = json.optBoolean("fastCDVD", def.fastCDVD),
                intcStat = json.optBoolean("intcStat", def.intcStat),
                waitLoop = json.optBoolean("waitLoop", def.waitLoop),
                vuNeonFusions = json.optBoolean("vuNeonFusions", def.vuNeonFusions),
                vuDeferredWrites = json.optBoolean("vuDeferredWrites", def.vuDeferredWrites),
                vuSkipStallSim = json.optBoolean("vuSkipStallSim", def.vuSkipStallSim),
                frameLimitEnable = json.optBoolean("frameLimitEnable", def.frameLimitEnable),
                aspectRatio = json.optInt("aspectRatio", def.aspectRatio),
                dev9EthEnable = json.optBoolean("dev9EthEnable", def.dev9EthEnable),
                dev9EthApi = json.optString("dev9EthApi", def.dev9EthApi).ifEmpty { def.dev9EthApi },
                dev9EthDevice = json.optString("dev9EthDevice", def.dev9EthDevice).ifEmpty { def.dev9EthDevice },
                dev9HddEnable = json.optBoolean("dev9HddEnable", def.dev9HddEnable),
                dev9HddFile = json.optString("dev9HddFile", def.dev9HddFile).ifEmpty { def.dev9HddFile },
                recEE = json.optBoolean("recEE", def.recEE),
                recIOP = json.optBoolean("recIOP", def.recIOP),
                recVU0 = json.optBoolean("recVU0", def.recVU0),
                recVU1 = json.optBoolean("recVU1", def.recVU1),
                enableFastmem = json.optBoolean("enableFastmem", def.enableFastmem),
                useMacEE = true,
                useMacIOP = true,
                useMacVU0 = true,
                useMacVU1 = true,
                vu1InlineFmacStall = json.optBoolean("vu1InlineFmacStall", def.vu1InlineFmacStall),
                vu1CrossBlockPState = json.optBoolean("vu1CrossBlockPState", def.vu1CrossBlockPState),
                vu1InlineDrainTestPipes = json.optBoolean("vu1InlineDrainTestPipes", def.vu1InlineDrainTestPipes),
                vu1FmacInstanceRouting = json.optBoolean("vu1FmacInstanceRouting", def.vu1FmacInstanceRouting),
                hwMipmap = json.optBoolean("hwMipmap", def.hwMipmap),
                accurateBlendingUnit = json.optInt("accurateBlendingUnit", def.accurateBlendingUnit),
                textureFiltering = json.optInt("textureFiltering", def.textureFiltering),
                texturePreloading = json.optInt("texturePreloading", def.texturePreloading),
                loadTextureReplacements = json.optBoolean("loadTextureReplacements", def.loadTextureReplacements),
                loadTextureReplacementsAsync = json.optBoolean("loadTextureReplacementsAsync", def.loadTextureReplacementsAsync),
                precacheTextureReplacements = json.optBoolean("precacheTextureReplacements", def.precacheTextureReplacements),
                dumpReplaceableTextures = json.optBoolean("dumpReplaceableTextures", def.dumpReplaceableTextures),
                osdShowTextureReplacements = json.optBoolean("osdShowTextureReplacements", def.osdShowTextureReplacements),
                autoFlush = json.optInt("autoFlush", def.autoFlush),
                halfPixelOffset = json.optInt("halfPixelOffset", def.halfPixelOffset),
                triFilter = json.optInt("triFilter", def.triFilter),
                maxAnisotropy = json.optInt("maxAnisotropy", def.maxAnisotropy),
                gpuProfile = json.optInt("gpuProfile", def.gpuProfile),
            )
        }

        /** Treat any field present in [overrides] as a delta over [base]. */
        /**
         * Compute the sparse override JSON between two Settings: returns
         * only fields where `current` differs from `base`. Used by the
         * overlay's per-game save path so we only persist what the user
         * actually changed for this title — global tweaks still flow
         * through fields the user hasn't touched. Mirrors the field set
         * of [merge] above (must stay in sync).
         */
        fun diff(base: Settings, current: Settings): JSONObject {
            val j = JSONObject()
            if (current.eeCycleRate         != base.eeCycleRate)         j.put("eeCycleRate", current.eeCycleRate)
            if (current.eeCycleSkip         != base.eeCycleSkip)         j.put("eeCycleSkip", current.eeCycleSkip)
            if (current.vu1Instant          != base.vu1Instant)          j.put("vu1Instant", current.vu1Instant)
            if (current.vuFlagHack          != base.vuFlagHack)          j.put("vuFlagHack", current.vuFlagHack)
            if (current.fastCDVD            != base.fastCDVD)            j.put("fastCDVD", current.fastCDVD)
            if (current.intcStat            != base.intcStat)            j.put("intcStat", current.intcStat)
            if (current.waitLoop            != base.waitLoop)            j.put("waitLoop", current.waitLoop)
            if (current.vuNeonFusions       != base.vuNeonFusions)       j.put("vuNeonFusions", current.vuNeonFusions)
            if (current.vuDeferredWrites    != base.vuDeferredWrites)    j.put("vuDeferredWrites", current.vuDeferredWrites)
            if (current.vuSkipStallSim      != base.vuSkipStallSim)      j.put("vuSkipStallSim", current.vuSkipStallSim)
            if (current.frameLimitEnable    != base.frameLimitEnable)    j.put("frameLimitEnable", current.frameLimitEnable)
            if (current.aspectRatio         != base.aspectRatio)         j.put("aspectRatio", current.aspectRatio)
            if (current.dev9EthEnable       != base.dev9EthEnable)       j.put("dev9EthEnable", current.dev9EthEnable)
            if (current.dev9EthApi          != base.dev9EthApi)          j.put("dev9EthApi", current.dev9EthApi)
            if (current.dev9EthDevice       != base.dev9EthDevice)       j.put("dev9EthDevice", current.dev9EthDevice)
            if (current.dev9HddEnable       != base.dev9HddEnable)       j.put("dev9HddEnable", current.dev9HddEnable)
            if (current.dev9HddFile         != base.dev9HddFile)         j.put("dev9HddFile", current.dev9HddFile)
            if (current.recEE               != base.recEE)               j.put("recEE", current.recEE)
            if (current.recIOP              != base.recIOP)              j.put("recIOP", current.recIOP)
            if (current.recVU0              != base.recVU0)              j.put("recVU0", current.recVU0)
            if (current.recVU1              != base.recVU1)              j.put("recVU1", current.recVU1)
            if (current.enableFastmem       != base.enableFastmem)       j.put("enableFastmem", current.enableFastmem)
            if (current.vu1InlineFmacStall  != base.vu1InlineFmacStall)  j.put("vu1InlineFmacStall", current.vu1InlineFmacStall)
            if (current.vu1CrossBlockPState != base.vu1CrossBlockPState) j.put("vu1CrossBlockPState", current.vu1CrossBlockPState)
            if (current.vu1InlineDrainTestPipes != base.vu1InlineDrainTestPipes) j.put("vu1InlineDrainTestPipes", current.vu1InlineDrainTestPipes)
            if (current.vu1FmacInstanceRouting != base.vu1FmacInstanceRouting) j.put("vu1FmacInstanceRouting", current.vu1FmacInstanceRouting)
            if (current.hwMipmap            != base.hwMipmap)            j.put("hwMipmap", current.hwMipmap)
            if (current.accurateBlendingUnit!= base.accurateBlendingUnit)j.put("accurateBlendingUnit", current.accurateBlendingUnit)
            if (current.textureFiltering    != base.textureFiltering)    j.put("textureFiltering", current.textureFiltering)
            if (current.texturePreloading   != base.texturePreloading)   j.put("texturePreloading", current.texturePreloading)
            if (current.loadTextureReplacements != base.loadTextureReplacements) j.put("loadTextureReplacements", current.loadTextureReplacements)
            if (current.loadTextureReplacementsAsync != base.loadTextureReplacementsAsync) j.put("loadTextureReplacementsAsync", current.loadTextureReplacementsAsync)
            if (current.precacheTextureReplacements != base.precacheTextureReplacements) j.put("precacheTextureReplacements", current.precacheTextureReplacements)
            if (current.dumpReplaceableTextures != base.dumpReplaceableTextures) j.put("dumpReplaceableTextures", current.dumpReplaceableTextures)
            if (current.osdShowTextureReplacements != base.osdShowTextureReplacements) j.put("osdShowTextureReplacements", current.osdShowTextureReplacements)
            if (current.autoFlush           != base.autoFlush)           j.put("autoFlush", current.autoFlush)
            if (current.halfPixelOffset     != base.halfPixelOffset)     j.put("halfPixelOffset", current.halfPixelOffset)
            if (current.triFilter           != base.triFilter)           j.put("triFilter", current.triFilter)
            if (current.maxAnisotropy       != base.maxAnisotropy)       j.put("maxAnisotropy", current.maxAnisotropy)
            if (current.gpuProfile          != base.gpuProfile)          j.put("gpuProfile", current.gpuProfile)
            return j
        }

        fun merge(base: Settings, overrides: JSONObject): Settings = Settings(
            eeCycleRate = if (overrides.has("eeCycleRate")) overrides.getInt("eeCycleRate") else base.eeCycleRate,
            eeCycleSkip = if (overrides.has("eeCycleSkip")) overrides.getInt("eeCycleSkip") else base.eeCycleSkip,
            mtvu = true,
            vu1Instant = if (overrides.has("vu1Instant")) overrides.getBoolean("vu1Instant") else base.vu1Instant,
            vuFlagHack = if (overrides.has("vuFlagHack")) overrides.getBoolean("vuFlagHack") else base.vuFlagHack,
            fastCDVD = if (overrides.has("fastCDVD")) overrides.getBoolean("fastCDVD") else base.fastCDVD,
            intcStat = if (overrides.has("intcStat")) overrides.getBoolean("intcStat") else base.intcStat,
            waitLoop = if (overrides.has("waitLoop")) overrides.getBoolean("waitLoop") else base.waitLoop,
            vuNeonFusions = if (overrides.has("vuNeonFusions")) overrides.getBoolean("vuNeonFusions") else base.vuNeonFusions,
            vuDeferredWrites = if (overrides.has("vuDeferredWrites")) overrides.getBoolean("vuDeferredWrites") else base.vuDeferredWrites,
            vuSkipStallSim = if (overrides.has("vuSkipStallSim")) overrides.getBoolean("vuSkipStallSim") else base.vuSkipStallSim,
            frameLimitEnable = if (overrides.has("frameLimitEnable")) overrides.getBoolean("frameLimitEnable") else base.frameLimitEnable,
            aspectRatio = if (overrides.has("aspectRatio")) overrides.getInt("aspectRatio") else base.aspectRatio,
            dev9EthEnable = if (overrides.has("dev9EthEnable")) overrides.getBoolean("dev9EthEnable") else base.dev9EthEnable,
            dev9EthApi = if (overrides.has("dev9EthApi")) overrides.getString("dev9EthApi").ifEmpty { base.dev9EthApi } else base.dev9EthApi,
            dev9EthDevice = if (overrides.has("dev9EthDevice")) overrides.getString("dev9EthDevice").ifEmpty { base.dev9EthDevice } else base.dev9EthDevice,
            dev9HddEnable = if (overrides.has("dev9HddEnable")) overrides.getBoolean("dev9HddEnable") else base.dev9HddEnable,
            dev9HddFile = if (overrides.has("dev9HddFile")) overrides.getString("dev9HddFile").ifEmpty { base.dev9HddFile } else base.dev9HddFile,
            recEE = if (overrides.has("recEE")) overrides.getBoolean("recEE") else base.recEE,
            recIOP = if (overrides.has("recIOP")) overrides.getBoolean("recIOP") else base.recIOP,
            recVU0 = if (overrides.has("recVU0")) overrides.getBoolean("recVU0") else base.recVU0,
            recVU1 = if (overrides.has("recVU1")) overrides.getBoolean("recVU1") else base.recVU1,
            enableFastmem = if (overrides.has("enableFastmem")) overrides.getBoolean("enableFastmem") else base.enableFastmem,
            useMacEE = true,
            useMacIOP = true,
            useMacVU0 = true,
            useMacVU1 = true,
            vu1InlineFmacStall = if (overrides.has("vu1InlineFmacStall")) overrides.getBoolean("vu1InlineFmacStall") else base.vu1InlineFmacStall,
            vu1CrossBlockPState = if (overrides.has("vu1CrossBlockPState")) overrides.getBoolean("vu1CrossBlockPState") else base.vu1CrossBlockPState,
            vu1InlineDrainTestPipes = if (overrides.has("vu1InlineDrainTestPipes")) overrides.getBoolean("vu1InlineDrainTestPipes") else base.vu1InlineDrainTestPipes,
            vu1FmacInstanceRouting = if (overrides.has("vu1FmacInstanceRouting")) overrides.getBoolean("vu1FmacInstanceRouting") else base.vu1FmacInstanceRouting,
            hwMipmap = if (overrides.has("hwMipmap")) overrides.getBoolean("hwMipmap") else base.hwMipmap,
            accurateBlendingUnit = if (overrides.has("accurateBlendingUnit")) overrides.getInt("accurateBlendingUnit") else base.accurateBlendingUnit,
            textureFiltering = if (overrides.has("textureFiltering")) overrides.getInt("textureFiltering") else base.textureFiltering,
            texturePreloading = if (overrides.has("texturePreloading")) overrides.getInt("texturePreloading") else base.texturePreloading,
            loadTextureReplacements = if (overrides.has("loadTextureReplacements")) overrides.getBoolean("loadTextureReplacements") else base.loadTextureReplacements,
            loadTextureReplacementsAsync = if (overrides.has("loadTextureReplacementsAsync")) overrides.getBoolean("loadTextureReplacementsAsync") else base.loadTextureReplacementsAsync,
            precacheTextureReplacements = if (overrides.has("precacheTextureReplacements")) overrides.getBoolean("precacheTextureReplacements") else base.precacheTextureReplacements,
            dumpReplaceableTextures = if (overrides.has("dumpReplaceableTextures")) overrides.getBoolean("dumpReplaceableTextures") else base.dumpReplaceableTextures,
            osdShowTextureReplacements = if (overrides.has("osdShowTextureReplacements")) overrides.getBoolean("osdShowTextureReplacements") else base.osdShowTextureReplacements,
            autoFlush = if (overrides.has("autoFlush")) overrides.getInt("autoFlush") else base.autoFlush,
            halfPixelOffset = if (overrides.has("halfPixelOffset")) overrides.getInt("halfPixelOffset") else base.halfPixelOffset,
            triFilter = if (overrides.has("triFilter")) overrides.getInt("triFilter") else base.triFilter,
            maxAnisotropy = if (overrides.has("maxAnisotropy")) overrides.getInt("maxAnisotropy") else base.maxAnisotropy,
            gpuProfile = if (overrides.has("gpuProfile")) overrides.getInt("gpuProfile") else base.gpuProfile,
        )
    }
}
