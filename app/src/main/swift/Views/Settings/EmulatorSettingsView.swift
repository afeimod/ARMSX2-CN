// EmulatorSettingsView.swift — EE/IOP/VU/boot/speedhack settings
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct EmulatorSettingsView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section {
                Toggle(isOn: Binding(
                    get: { settings.eeCoreType != 1 },
                    set: { settings.eeCoreType = $0 ? 2 : 1 }
                )) {
                    HStack {
                        Text("EE Core")
                        Spacer()
                        Text(settings.eeCoreType != 1 ? "ARM64 JIT" : "Interpreter")
                            .foregroundStyle(.secondary)
                            .font(.callout)
                    }
                }
                Toggle(isOn: $settings.iopRecompiler) {
                    HStack {
                        Text("IOP")
                        Spacer()
                        Text(settings.iopRecompiler ? "JIT" : "Interpreter")
                            .foregroundStyle(.secondary)
                            .font(.callout)
                    }
                }
                Toggle(isOn: $settings.vu0Recompiler) {
                    HStack {
                        Text("VU0")
                        Spacer()
                        Text(settings.vu0Recompiler ? "JIT" : "Interpreter")
                            .foregroundStyle(.secondary)
                            .font(.callout)
                    }
                }
                Toggle(isOn: $settings.vu1Recompiler) {
                    HStack {
                        Text("VU1")
                        Spacer()
                        Text(settings.vu1Recompiler ? "JIT" : "Interpreter")
                            .foregroundStyle(.secondary)
                            .font(.callout)
                    }
                }
                Text("Changes take effect on next VM boot.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("CPU Recompiler")
            }

            Section("Boot") {
                Toggle("Fast Boot", isOn: $settings.fastBoot)
                Text("Skips BIOS intro. Some games require this OFF.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Memory") {
                Toggle("Fastmem", isOn: $settings.fastmem)
                Text("Direct memory mapping for EE. Disable if 3D graphics are broken. Requires restart.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Performance") {
                Toggle("Frame Limiter", isOn: $settings.frameLimiterEnabled)

                if settings.frameLimiterEnabled {
                    HStack {
                        Text("Speed Target")
                        Spacer()
                        Text("Normal")
                            .foregroundStyle(.secondary)
                            .font(.callout.monospacedDigit())
                    }
                } else {
                    HStack {
                        Text("Speed Target")
                        Spacer()
                        Text("Unlocked")
                            .foregroundStyle(.orange)
                            .font(.callout.monospacedDigit())
                    }
                }

                HStack {
                    Text("NTSC Base Rate")
                    Spacer()
                    Text(Self.formatFPS(settings.ntscFramerate))
                        .foregroundStyle(.secondary)
                        .font(.callout.monospacedDigit())
                }

                HStack {
                    Text("PAL Base Rate")
                    Spacer()
                    Text(Self.formatFPS(settings.palFramerate))
                        .foregroundStyle(.secondary)
                        .font(.callout.monospacedDigit())
                }

                Text("Frame Limiter controls emulator speed, not a safe 30 FPS display cap. Keep it ON for normal PS2 timing; OFF unlocks speed and can increase heat and battery drain.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                Button("Use VU1 Interpreter Preset") {
                    settings.applyVU1CompatibilityPreset()
                }
                Button("Use Full Interpreter Preset") {
                    settings.applyFullInterpreterPreset()
                }
                Text("Use the VU1 preset first for boot crashes or VU1-related texture/rendering glitches. Full Interpreter is much slower, but helps isolate dynarec/JIT issues.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Compatibility")
            } footer: {
                Text("Changes take effect on next VM boot.")
            }

            Section("Patches & Cheats") {
                Toggle("Enable GameDB Patches", isOn: $settings.enablePatches)
                Toggle("Enable PNACH Cheats", isOn: $settings.enableCheats)
                Toggle("Widescreen Patches", isOn: $settings.enableWidescreenPatches)
                Toggle("No-Interlacing Patches", isOn: $settings.enableNoInterlacingPatches)

                Text("PNACH cheats and 60 FPS patches can be imported from the in-game quick menu or from a game's long-press menu. They are renamed to Serial_CRC.pnach automatically.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                Stepper("EE Cycle Rate: \(settings.eeCycleRate)", value: $settings.eeCycleRate, in: -3...3)
                Text("0 = Default. Negative = underclock (stable). Positive = overclock (fast but risky).")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle("Fast CDVD", isOn: $settings.fastCDVD)
                Toggle("VU1 Instant", isOn: $settings.vu1Instant)
                Toggle("MTVU", isOn: $settings.mtvu)
                Toggle("Wait Loop Detection", isOn: $settings.waitLoop)
                Toggle("INTC Stat Hack", isOn: $settings.intcStat)

                Text("VU1 Instant and MTVU are independent now. MTVU can help some games, but keep it off unless a game specifically benefits on iOS.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Speedhacks")
            } footer: {
                Text("Changes take effect on next VM boot.")
            }

            Section {
                Button("Reset Emulator to Defaults") {
                    settings.resetEmulatorDefaults()
                }
                .foregroundStyle(.red)
            }
        }
        .navigationTitle("Emulator")
        .navigationBarTitleDisplayMode(.inline)
    }

    private static func formatFPS(_ value: Float) -> String {
        String(format: "%.2f FPS", value)
    }
}
