// EmulatorSettingsView.swift — EE/IOP/VU/boot/speedhack settings
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct EmulatorSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var stikDebugOpenFailed = false
    @State private var stikDebugOpenInProgress = false

    var body: some View {
        Form {
            Section {
                Toggle(isOn: Binding(
                    get: { settings.eeCoreType != 1 },
                    set: { settings.eeCoreType = $0 ? 2 : 1 }
                )) {
                    HStack {
                        Text(settings.localized("EE Core"))
                        Spacer()
                        Text(settings.localized(settings.eeCoreType != 1 ? "ARM64 JIT" : "Interpreter"))
                            .foregroundStyle(.secondary)
                            .font(.callout)
                    }
                }
                Toggle(isOn: $settings.iopRecompiler) {
                    HStack {
                        Text("IOP")
                        Spacer()
                        Text(settings.localized(settings.iopRecompiler ? "JIT" : "Interpreter"))
                            .foregroundStyle(.secondary)
                            .font(.callout)
                    }
                }
                Toggle(isOn: $settings.vu0Recompiler) {
                    HStack {
                        Text("VU0")
                        Spacer()
                        Text(settings.localized(settings.vu0Recompiler ? "JIT" : "Interpreter"))
                            .foregroundStyle(.secondary)
                            .font(.callout)
                    }
                }
                Toggle(isOn: $settings.vu1Recompiler) {
                    HStack {
                        Text("VU1")
                        Spacer()
                        Text(settings.localized(settings.vu1Recompiler ? "JIT" : "Interpreter"))
                            .foregroundStyle(.secondary)
                            .font(.callout)
                    }
                }
                Text(settings.localized("Changes take effect on next VM boot."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(settings.localized("CPU Recompiler"))
            }

            Section(settings.localized("StikDebug")) {
                Toggle(settings.localized("Auto-open StikDebug"), isOn: $settings.autoOpenStikDebug)

                Picker(settings.localized("JIT Script"), selection: $settings.jitScriptProtocol) {
                    ForEach(JITScriptProtocol.allCases) { scriptProtocol in
                        Text(settings.localized(scriptProtocol.label)).tag(scriptProtocol)
                    }
                }
                .pickerStyle(.segmented)

                Text(settings.localized(settings.jitScriptProtocol.subtitle))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Button {
                    stikDebugOpenInProgress = true
                    stikDebugOpenFailed = false
                    StikDebugLauncher.open(reason: "emulator-settings") { success in
                        stikDebugOpenInProgress = false
                        stikDebugOpenFailed = !success
                    }
                } label: {
                    Label(settings.localized("Open StikDebug"), systemImage: "bolt.horizontal.circle")
                }
                .disabled(stikDebugOpenInProgress)

                Text(settings.localized("Select the same script here that you run in StikDebug. This only changes the debugger breakpoint protocol used to prepare JIT memory. Fully close and relaunch ARMSX2 after switching scripts."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                if stikDebugOpenFailed {
                    Text(settings.localized("Open StikDebug manually, then run the selected script and relaunch ARMSX2."))
                        .font(.caption)
                        .foregroundStyle(.orange)
                }
            }

            Section(settings.localized("Boot")) {
                Toggle(settings.localized("Fast Boot"), isOn: $settings.fastBoot)
                Text(settings.localized("Skips BIOS intro. Some games require this OFF."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Host Filesystem")) {
                Toggle(settings.localized("Enable Host Filesystem"), isOn: $settings.hostFilesystem)
                Text(settings.localized("Allows PS2 homebrew and ELF tools to access files through the host: device. This is separate from USB/SSD game storage, is off by default, and takes effect on next VM boot."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Memory")) {
                Toggle(settings.localized("Fastmem"), isOn: $settings.fastmem)
                Text(settings.localized("Direct memory mapping for EE. Disable if 3D graphics are broken. Requires restart."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Audio")) {
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        Text(settings.localized("Emulator Volume"))
                        Spacer()
                        Text(Self.formatPercent(settings.emulatorVolumePercent))
                            .foregroundStyle(.secondary)
                            .font(.callout.monospacedDigit())
                    }

                    Slider(
                        value: Binding(
                            get: { Double(settings.emulatorVolumePercent) },
                            set: { settings.emulatorVolumePercent = Int($0.rounded()) }
                        ),
                        in: 0...100,
                        step: 1
                    )
                    .accessibilityLabel(settings.localized("Emulator Volume"))
                    .accessibilityValue(Self.formatPercent(settings.emulatorVolumePercent))
                    .accessibilityHint(settings.localized("Adjusts emulator game audio without changing iOS system volume or other apps."))

                    HStack {
                        Text("0%")
                        Spacer()
                        Button(settings.localized("Reset")) {
                            settings.emulatorVolumePercent = SettingsStore.defaultEmulatorVolumePercent
                        }
                        .buttonStyle(.borderless)
                        Spacer()
                        Text("100%")
                    }
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
                }

                Text(settings.localized("Controls emulator and game audio only. iOS system volume and other apps stay separate."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Performance")) {
                Toggle(settings.localized("Frame Limiter"), isOn: $settings.frameLimiterEnabled)

                if settings.frameLimiterEnabled {
                    VStack(alignment: .leading, spacing: 8) {
                        HStack {
                            Text(settings.localized("FPS Target"))
                            Spacer()
                            Text(Self.formatFPS(settings.targetFPS))
                                .foregroundStyle(.secondary)
                                .font(.callout.monospacedDigit())
                        }

                        Slider(
                            value: $settings.targetFPS,
                            in: SettingsStore.minTargetFPS...SettingsStore.maxTargetFPS,
                            step: 1.0
                        )

                        HStack {
                            Text(Self.formatFPS(SettingsStore.minTargetFPS))
                            Spacer()
                            Button(settings.localized("60 FPS")) {
                                settings.targetFPS = SettingsStore.defaultTargetFPS
                            }
                            .buttonStyle(.borderless)
                            Spacer()
                            Text(Self.formatFPS(SettingsStore.maxTargetFPS))
                        }
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                    }
                } else {
                    HStack {
                        Text(settings.localized("Speed Target"))
                        Spacer()
                        Text(settings.localized("Unlocked"))
                            .foregroundStyle(.orange)
                            .font(.callout.monospacedDigit())
                    }
                }

                HStack {
                    Text(settings.localized("NTSC Base Rate"))
                    Spacer()
                    Text(Self.formatFPS(settings.ntscFramerate))
                        .foregroundStyle(.secondary)
                        .font(.callout.monospacedDigit())
                }

                HStack {
                    Text(settings.localized("PAL Base Rate"))
                    Spacer()
                    Text(Self.formatFPS(settings.palFramerate))
                        .foregroundStyle(.secondary)
                        .font(.callout.monospacedDigit())
                }

                Text(settings.localized("FPS Target maps to PCSX2 Normal Speed: 60 FPS is normal NTSC timing, 30 FPS is about 50% speed, and higher values fast-forward. Turning the limiter OFF unlocks speed and can increase heat and battery drain."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                Button(settings.localized("Use VU1 Interpreter Preset")) {
                    settings.applyVU1CompatibilityPreset()
                }
                Button(settings.localized("Use Full Interpreter Preset")) {
                    settings.applyFullInterpreterPreset()
                }
                Text(settings.localized("Use the VU1 preset first for boot crashes or VU1-related texture/rendering glitches. Full Interpreter is much slower, but helps isolate dynarec/JIT issues."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(settings.localized("Compatibility"))
            } footer: {
                Text(settings.localized("Changes take effect on next VM boot."))
            }

            Section {
                Toggle(settings.localized("GameDB Automatic Fixes"), isOn: Binding(
                    get: { settings.enableGameFixes && settings.enableGameDBHardwareFixes },
                    set: { enabled in
                        settings.enableGameFixes = enabled
                        settings.enableGameDBHardwareFixes = enabled
                    }
                ))
                Toggle(settings.localized("GameDB Core Fixes"), isOn: $settings.enableGameFixes)
                Toggle(settings.localized("GameDB Graphics Fixes"), isOn: $settings.enableGameDBHardwareFixes)
                Toggle(settings.localized("GameDB PNACH Patches"), isOn: $settings.enablePatches)
                Toggle(settings.localized("Enable PNACH Cheats"), isOn: $settings.enableCheats)
                Toggle(settings.localized("Widescreen Patches"), isOn: $settings.enableWidescreenPatches)
                Toggle(settings.localized("No-Interlacing Patches"), isOn: $settings.enableNoInterlacingPatches)

                Text(settings.localized("GameDB Core Fixes covers timing, clamps, and gamefixes. GameDB Graphics Fixes covers renderer-specific hardware fixes; turn it off globally or per-game if a title looks worse on Metal. Use Cheats & Patches from the in-game quick menu or a game's long-press menu to import and manage patch files."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(settings.localized("Patches & Cheats"))
            } footer: {
                Text(settings.localized("Changes take effect on next VM boot."))
            }

            Section {
                Stepper("\(settings.localized("EE Cycle Rate")): \(settings.eeCycleRate)", value: $settings.eeCycleRate, in: -3...3)
                Text(settings.localized("0 = Default. Negative = underclock (stable). Positive = overclock (fast but risky)."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle(settings.localized("Fast CDVD"), isOn: $settings.fastCDVD)
                Toggle(settings.localized("VU1 Instant"), isOn: $settings.vu1Instant)
                Toggle("MTVU", isOn: $settings.mtvu)
                Toggle(settings.localized("Wait Loop Detection"), isOn: $settings.waitLoop)
                Toggle(settings.localized("INTC Stat Hack"), isOn: $settings.intcStat)

                Text(settings.localized("VU1 Instant and MTVU are independent now. MTVU can help some games, but keep it off unless a game specifically benefits on iOS."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text(settings.localized("Speedhacks"))
            } footer: {
                Text(settings.localized("Changes take effect on next VM boot."))
            }

            Section {
                Button(settings.localized("Reset Emulator to Defaults")) {
                    settings.resetEmulatorDefaults()
                }
                .foregroundStyle(.red)
            }
        }
        .navigationTitle(settings.localized("Emulator"))
        .navigationBarTitleDisplayMode(.inline)
    }

    private static func formatFPS(_ value: Float) -> String {
        String(format: "%.2f FPS", value)
    }

    private static func formatPercent(_ value: Int) -> String {
        "\(SettingsStore.clampedEmulatorVolumePercent(value))%"
    }
}
