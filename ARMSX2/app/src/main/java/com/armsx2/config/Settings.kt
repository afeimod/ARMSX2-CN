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

    // ---- EmuCore/GS — frame limiter ----
    /** EmuCore/GS/FrameLimitEnable. */
    val frameLimitEnable: Boolean = true,

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
        // GS frame limit
        NativeApp.setSetting("EmuCore/GS", "FrameLimitEnable", "bool", frameLimitEnable.toString())
        // GS renderer
        NativeApp.setSetting("EmuCore/GS", "hw_mipmap", "bool", hwMipmap.toString())
        NativeApp.setSetting("EmuCore/GS", "accurate_blending_unit", "int", accurateBlendingUnit.toString())
        NativeApp.setSetting("EmuCore/GS", "filter", "int", textureFiltering.toString())
        NativeApp.setSetting("EmuCore/GS", "texture_preloading", "int", texturePreloading.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_AutoFlush", "int", autoFlush.toString())
        NativeApp.setSetting("EmuCore/GS", "UserHacks_HalfPixelOffset", "int", halfPixelOffset.toString())
        NativeApp.setSetting("EmuCore/GS", "TriFilter", "int", triFilter.toString())
        NativeApp.setSetting("EmuCore/GS", "MaxAnisotropy", "int", maxAnisotropy.toString())
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
        put("frameLimitEnable", frameLimitEnable)
        put("hwMipmap", hwMipmap)
        put("accurateBlendingUnit", accurateBlendingUnit)
        put("textureFiltering", textureFiltering)
        put("texturePreloading", texturePreloading)
        put("autoFlush", autoFlush)
        put("halfPixelOffset", halfPixelOffset)
        put("triFilter", triFilter)
        put("maxAnisotropy", maxAnisotropy)
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
                frameLimitEnable = json.optBoolean("frameLimitEnable", def.frameLimitEnable),
                hwMipmap = json.optBoolean("hwMipmap", def.hwMipmap),
                accurateBlendingUnit = json.optInt("accurateBlendingUnit", def.accurateBlendingUnit),
                textureFiltering = json.optInt("textureFiltering", def.textureFiltering),
                texturePreloading = json.optInt("texturePreloading", def.texturePreloading),
                autoFlush = json.optInt("autoFlush", def.autoFlush),
                halfPixelOffset = json.optInt("halfPixelOffset", def.halfPixelOffset),
                triFilter = json.optInt("triFilter", def.triFilter),
                maxAnisotropy = json.optInt("maxAnisotropy", def.maxAnisotropy),
            )
        }

        /** Treat any field present in [overrides] as a delta over [base]. */
        fun merge(base: Settings, overrides: JSONObject): Settings = Settings(
            eeCycleRate = if (overrides.has("eeCycleRate")) overrides.getInt("eeCycleRate") else base.eeCycleRate,
            eeCycleSkip = if (overrides.has("eeCycleSkip")) overrides.getInt("eeCycleSkip") else base.eeCycleSkip,
            mtvu = if (overrides.has("mtvu")) overrides.getBoolean("mtvu") else base.mtvu,
            vu1Instant = if (overrides.has("vu1Instant")) overrides.getBoolean("vu1Instant") else base.vu1Instant,
            vuFlagHack = if (overrides.has("vuFlagHack")) overrides.getBoolean("vuFlagHack") else base.vuFlagHack,
            fastCDVD = if (overrides.has("fastCDVD")) overrides.getBoolean("fastCDVD") else base.fastCDVD,
            intcStat = if (overrides.has("intcStat")) overrides.getBoolean("intcStat") else base.intcStat,
            waitLoop = if (overrides.has("waitLoop")) overrides.getBoolean("waitLoop") else base.waitLoop,
            frameLimitEnable = if (overrides.has("frameLimitEnable")) overrides.getBoolean("frameLimitEnable") else base.frameLimitEnable,
            hwMipmap = if (overrides.has("hwMipmap")) overrides.getBoolean("hwMipmap") else base.hwMipmap,
            accurateBlendingUnit = if (overrides.has("accurateBlendingUnit")) overrides.getInt("accurateBlendingUnit") else base.accurateBlendingUnit,
            textureFiltering = if (overrides.has("textureFiltering")) overrides.getInt("textureFiltering") else base.textureFiltering,
            texturePreloading = if (overrides.has("texturePreloading")) overrides.getInt("texturePreloading") else base.texturePreloading,
            autoFlush = if (overrides.has("autoFlush")) overrides.getInt("autoFlush") else base.autoFlush,
            halfPixelOffset = if (overrides.has("halfPixelOffset")) overrides.getInt("halfPixelOffset") else base.halfPixelOffset,
            triFilter = if (overrides.has("triFilter")) overrides.getInt("triFilter") else base.triFilter,
            maxAnisotropy = if (overrides.has("maxAnisotropy")) overrides.getInt("maxAnisotropy") else base.maxAnisotropy,
        )
    }
}
