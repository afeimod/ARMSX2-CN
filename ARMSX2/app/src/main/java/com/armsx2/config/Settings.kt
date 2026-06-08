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
    /** EmuCore/Speedhacks/vuThread — Multi-Threaded VU1 (MTVU). */
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

    // ---- macOS-port arm64 backend A/B toggles ----
    // Per-CPU switch between our original arm64 backend (default) and the
    // namespaced macOS-port backend (pcsx2_macrec). VMManager picks at VM
    // init; flipping requires a VM restart. Bisect a mac regression by
    // flipping individual CPUs to original.
    /** EmuCore/CPU/Recompiler/UseMacEE — mac arm64 EE recompiler. */
    val useMacEE: Boolean = false,
    /** EmuCore/CPU/Recompiler/UseMacIOP — mac arm64 IOP recompiler. */
    val useMacIOP: Boolean = false,
    /** EmuCore/CPU/Recompiler/UseMacVU0 — mac arm64 VU0 recompiler. */
    val useMacVU0: Boolean = false,
    /** EmuCore/CPU/Recompiler/UseMacVU1 — mac arm64 VU1 recompiler. */
    val useMacVU1: Boolean = false,

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
    /** EmuCore/GS/UserHacks_AutoFlush — GSHWAutoFlushLevel:
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
        // Recompiler enables. Picked up by VMManager::ApplySettings →
        // SysCpuProviderPack rebind. Toggling these on a running VM swaps
        // the dispatch pointer; existing JIT block caches are flushed by
        // ApplySettings's CpusChanged path.
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "EnableEE", "bool", recEE.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "EnableIOP", "bool", recIOP.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "EnableVU0", "bool", recVU0.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "EnableVU1", "bool", recVU1.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "EnableFastmem", "bool", enableFastmem.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "UseMacEE", "bool", useMacEE.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "UseMacIOP", "bool", useMacIOP.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "UseMacVU0", "bool", useMacVU0.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "UseMacVU1", "bool", useMacVU1.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "Vu1InlineFmacStall", "bool", vu1InlineFmacStall.toString())
        NativeApp.setSetting("EmuCore/CPU/Recompiler", "Vu1CrossBlockPState", "bool", vu1CrossBlockPState.toString())
        // GS renderer
        NativeApp.setSetting("EmuCore/GS", "hw_mipmap", "bool", hwMipmap.toString())
        NativeApp.setSetting("EmuCore/GS", "accurate_blending_unit", "int", accurateBlendingUnit.toString())
        NativeApp.setSetting("EmuCore/GS", "filter", "int", textureFiltering.toString())
        NativeApp.setSetting("EmuCore/GS", "texture_preloading", "int", texturePreloading.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_AutoFlush", "int", autoFlush.toString())
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
        put("mtvu", mtvu)
        put("vu1Instant", vu1Instant)
        put("vuFlagHack", vuFlagHack)
        put("fastCDVD", fastCDVD)
        put("intcStat", intcStat)
        put("waitLoop", waitLoop)
        put("vuNeonFusions", vuNeonFusions)
        put("vuDeferredWrites", vuDeferredWrites)
        put("vuSkipStallSim", vuSkipStallSim)
        put("frameLimitEnable", frameLimitEnable)
        put("recEE", recEE)
        put("recIOP", recIOP)
        put("recVU0", recVU0)
        put("recVU1", recVU1)
        put("enableFastmem", enableFastmem)
        put("useMacEE", useMacEE)
        put("useMacIOP", useMacIOP)
        put("useMacVU0", useMacVU0)
        put("useMacVU1", useMacVU1)
        put("vu1InlineFmacStall", vu1InlineFmacStall)
        put("vu1CrossBlockPState", vu1CrossBlockPState)
        put("hwMipmap", hwMipmap)
        put("accurateBlendingUnit", accurateBlendingUnit)
        put("textureFiltering", textureFiltering)
        put("texturePreloading", texturePreloading)
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
                mtvu = json.optBoolean("mtvu", def.mtvu),
                vu1Instant = json.optBoolean("vu1Instant", def.vu1Instant),
                vuFlagHack = json.optBoolean("vuFlagHack", def.vuFlagHack),
                fastCDVD = json.optBoolean("fastCDVD", def.fastCDVD),
                intcStat = json.optBoolean("intcStat", def.intcStat),
                waitLoop = json.optBoolean("waitLoop", def.waitLoop),
                vuNeonFusions = json.optBoolean("vuNeonFusions", def.vuNeonFusions),
                vuDeferredWrites = json.optBoolean("vuDeferredWrites", def.vuDeferredWrites),
                vuSkipStallSim = json.optBoolean("vuSkipStallSim", def.vuSkipStallSim),
                frameLimitEnable = json.optBoolean("frameLimitEnable", def.frameLimitEnable),
                recEE = json.optBoolean("recEE", def.recEE),
                recIOP = json.optBoolean("recIOP", def.recIOP),
                recVU0 = json.optBoolean("recVU0", def.recVU0),
                recVU1 = json.optBoolean("recVU1", def.recVU1),
                enableFastmem = json.optBoolean("enableFastmem", def.enableFastmem),
                useMacEE = json.optBoolean("useMacEE", def.useMacEE),
                useMacIOP = json.optBoolean("useMacIOP", def.useMacIOP),
                useMacVU0 = json.optBoolean("useMacVU0", def.useMacVU0),
                useMacVU1 = json.optBoolean("useMacVU1", def.useMacVU1),
                vu1InlineFmacStall = json.optBoolean("vu1InlineFmacStall", def.vu1InlineFmacStall),
                vu1CrossBlockPState = json.optBoolean("vu1CrossBlockPState", def.vu1CrossBlockPState),
                hwMipmap = json.optBoolean("hwMipmap", def.hwMipmap),
                accurateBlendingUnit = json.optInt("accurateBlendingUnit", def.accurateBlendingUnit),
                textureFiltering = json.optInt("textureFiltering", def.textureFiltering),
                texturePreloading = json.optInt("texturePreloading", def.texturePreloading),
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
            if (current.mtvu                != base.mtvu)                j.put("mtvu", current.mtvu)
            if (current.vu1Instant          != base.vu1Instant)          j.put("vu1Instant", current.vu1Instant)
            if (current.vuFlagHack          != base.vuFlagHack)          j.put("vuFlagHack", current.vuFlagHack)
            if (current.fastCDVD            != base.fastCDVD)            j.put("fastCDVD", current.fastCDVD)
            if (current.intcStat            != base.intcStat)            j.put("intcStat", current.intcStat)
            if (current.waitLoop            != base.waitLoop)            j.put("waitLoop", current.waitLoop)
            if (current.vuNeonFusions       != base.vuNeonFusions)       j.put("vuNeonFusions", current.vuNeonFusions)
            if (current.vuDeferredWrites    != base.vuDeferredWrites)    j.put("vuDeferredWrites", current.vuDeferredWrites)
            if (current.vuSkipStallSim      != base.vuSkipStallSim)      j.put("vuSkipStallSim", current.vuSkipStallSim)
            if (current.frameLimitEnable    != base.frameLimitEnable)    j.put("frameLimitEnable", current.frameLimitEnable)
            if (current.recEE               != base.recEE)               j.put("recEE", current.recEE)
            if (current.recIOP              != base.recIOP)              j.put("recIOP", current.recIOP)
            if (current.recVU0              != base.recVU0)              j.put("recVU0", current.recVU0)
            if (current.recVU1              != base.recVU1)              j.put("recVU1", current.recVU1)
            if (current.enableFastmem       != base.enableFastmem)       j.put("enableFastmem", current.enableFastmem)
            if (current.useMacEE            != base.useMacEE)            j.put("useMacEE", current.useMacEE)
            if (current.useMacIOP           != base.useMacIOP)           j.put("useMacIOP", current.useMacIOP)
            if (current.useMacVU0           != base.useMacVU0)           j.put("useMacVU0", current.useMacVU0)
            if (current.useMacVU1           != base.useMacVU1)           j.put("useMacVU1", current.useMacVU1)
            if (current.vu1InlineFmacStall  != base.vu1InlineFmacStall)  j.put("vu1InlineFmacStall", current.vu1InlineFmacStall)
            if (current.vu1CrossBlockPState != base.vu1CrossBlockPState) j.put("vu1CrossBlockPState", current.vu1CrossBlockPState)
            if (current.hwMipmap            != base.hwMipmap)            j.put("hwMipmap", current.hwMipmap)
            if (current.accurateBlendingUnit!= base.accurateBlendingUnit)j.put("accurateBlendingUnit", current.accurateBlendingUnit)
            if (current.textureFiltering    != base.textureFiltering)    j.put("textureFiltering", current.textureFiltering)
            if (current.texturePreloading   != base.texturePreloading)   j.put("texturePreloading", current.texturePreloading)
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
            mtvu = if (overrides.has("mtvu")) overrides.getBoolean("mtvu") else base.mtvu,
            vu1Instant = if (overrides.has("vu1Instant")) overrides.getBoolean("vu1Instant") else base.vu1Instant,
            vuFlagHack = if (overrides.has("vuFlagHack")) overrides.getBoolean("vuFlagHack") else base.vuFlagHack,
            fastCDVD = if (overrides.has("fastCDVD")) overrides.getBoolean("fastCDVD") else base.fastCDVD,
            intcStat = if (overrides.has("intcStat")) overrides.getBoolean("intcStat") else base.intcStat,
            waitLoop = if (overrides.has("waitLoop")) overrides.getBoolean("waitLoop") else base.waitLoop,
            vuNeonFusions = if (overrides.has("vuNeonFusions")) overrides.getBoolean("vuNeonFusions") else base.vuNeonFusions,
            vuDeferredWrites = if (overrides.has("vuDeferredWrites")) overrides.getBoolean("vuDeferredWrites") else base.vuDeferredWrites,
            vuSkipStallSim = if (overrides.has("vuSkipStallSim")) overrides.getBoolean("vuSkipStallSim") else base.vuSkipStallSim,
            frameLimitEnable = if (overrides.has("frameLimitEnable")) overrides.getBoolean("frameLimitEnable") else base.frameLimitEnable,
            recEE = if (overrides.has("recEE")) overrides.getBoolean("recEE") else base.recEE,
            recIOP = if (overrides.has("recIOP")) overrides.getBoolean("recIOP") else base.recIOP,
            recVU0 = if (overrides.has("recVU0")) overrides.getBoolean("recVU0") else base.recVU0,
            recVU1 = if (overrides.has("recVU1")) overrides.getBoolean("recVU1") else base.recVU1,
            enableFastmem = if (overrides.has("enableFastmem")) overrides.getBoolean("enableFastmem") else base.enableFastmem,
            useMacEE = if (overrides.has("useMacEE")) overrides.getBoolean("useMacEE") else base.useMacEE,
            useMacIOP = if (overrides.has("useMacIOP")) overrides.getBoolean("useMacIOP") else base.useMacIOP,
            useMacVU0 = if (overrides.has("useMacVU0")) overrides.getBoolean("useMacVU0") else base.useMacVU0,
            useMacVU1 = if (overrides.has("useMacVU1")) overrides.getBoolean("useMacVU1") else base.useMacVU1,
            vu1InlineFmacStall = if (overrides.has("vu1InlineFmacStall")) overrides.getBoolean("vu1InlineFmacStall") else base.vu1InlineFmacStall,
            vu1CrossBlockPState = if (overrides.has("vu1CrossBlockPState")) overrides.getBoolean("vu1CrossBlockPState") else base.vu1CrossBlockPState,
            hwMipmap = if (overrides.has("hwMipmap")) overrides.getBoolean("hwMipmap") else base.hwMipmap,
            accurateBlendingUnit = if (overrides.has("accurateBlendingUnit")) overrides.getInt("accurateBlendingUnit") else base.accurateBlendingUnit,
            textureFiltering = if (overrides.has("textureFiltering")) overrides.getInt("textureFiltering") else base.textureFiltering,
            texturePreloading = if (overrides.has("texturePreloading")) overrides.getInt("texturePreloading") else base.texturePreloading,
            autoFlush = if (overrides.has("autoFlush")) overrides.getInt("autoFlush") else base.autoFlush,
            halfPixelOffset = if (overrides.has("halfPixelOffset")) overrides.getInt("halfPixelOffset") else base.halfPixelOffset,
            triFilter = if (overrides.has("triFilter")) overrides.getInt("triFilter") else base.triFilter,
            maxAnisotropy = if (overrides.has("maxAnisotropy")) overrides.getInt("maxAnisotropy") else base.maxAnisotropy,
            gpuProfile = if (overrides.has("gpuProfile")) overrides.getInt("gpuProfile") else base.gpuProfile,
        )
    }
}
