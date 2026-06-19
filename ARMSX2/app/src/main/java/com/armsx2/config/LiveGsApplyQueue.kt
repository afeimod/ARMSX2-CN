package com.armsx2.config

import kr.co.iefriends.pcsx2.NativeApp
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

object LiveGsApplyQueue {
    private val executor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "GSLiveApply").apply { isDaemon = true }
    }

    private val latestSettings = AtomicReference<Settings?>(null)
    private val settingsRunning = AtomicBoolean(false)
    private val latestUpscale = AtomicReference<Float?>(null)
    private val upscaleRunning = AtomicBoolean(false)

    fun applySettings(settings: Settings) {
        latestSettings.set(settings)
        if (settingsRunning.compareAndSet(false, true))
            executor.execute(::drainSettings)
    }

    fun applyUpscale(multiplier: Float) {
        latestUpscale.set(multiplier)
        if (upscaleRunning.compareAndSet(false, true))
            executor.execute(::drainUpscale)
    }

    private fun drainSettings() {
        try {
            while (true) {
                val settings = latestSettings.getAndSet(null) ?: break
                val applied = runCatching { settings.applyGsLive() }
                    .onFailure { println("@@ANDROID_GS_LIVE_QUEUE@@ settings_error=${it.javaClass.simpleName}") }
                    .getOrDefault(false)
                println("@@ANDROID_GS_LIVE_QUEUE@@ settings_applied=${if (applied) 1 else 0}")
            }
        } finally {
            settingsRunning.set(false)
            if (latestSettings.get() != null && settingsRunning.compareAndSet(false, true))
                executor.execute(::drainSettings)
        }
    }

    private fun drainUpscale() {
        try {
            while (true) {
                val multiplier = latestUpscale.getAndSet(null) ?: break
                runCatching { NativeApp.renderUpscalemultiplier(multiplier) }
                    .onFailure { println("@@ANDROID_GS_LIVE_QUEUE@@ upscale_error=${it.javaClass.simpleName}") }
                println("@@ANDROID_GS_LIVE_QUEUE@@ upscale value=$multiplier")
            }
        } finally {
            upscaleRunning.set(false)
            if (latestUpscale.get() != null && upscaleRunning.compareAndSet(false, true))
                executor.execute(::drainUpscale)
        }
    }
}
