// GraphicsSettingsView.swift — Renderer, upscale, filter, and display settings
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct GraphicsSettingsView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section(settings.localized("Renderer")) {
                Picker(settings.localized("Renderer"), selection: $settings.renderer) {
                    Text(settings.localized("Metal (Hardware)")).tag(17)
#if !targetEnvironment(macCatalyst)
                    Text(settings.localized("Software")).tag(13)
                    Text(settings.localized("Null (No Output)")).tag(11)
#endif
                }
#if targetEnvironment(macCatalyst)
                Text(settings.localized("Metal is required for the Mac Catalyst build. Requires restart."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
#else
                Text(settings.localized("Metal is the supported iOS renderer. Software is slow but useful for debugging. Null disables rendering. Requires restart."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
#endif
            }

            Section(settings.localized("Upscaling")) {
                Picker(settings.localized("Internal Resolution"), selection: $settings.upscaleMultiplier) {
                    Text(settings.localized("0.25x (Fastest)")).tag(Float(0.25))
                    Text("0.5x").tag(Float(0.5))
                    Text("0.75x").tag(Float(0.75))
                    Text(settings.localized("1x Native (512x448)")).tag(Float(1.0))
                    Text("2x (1024x896)").tag(Float(2.0))
                    Text("3x (1536x1344)").tag(Float(3.0))
                    Text("4x (2048x1792)").tag(Float(4.0))
                    Text("5x (2560x2240)").tag(Float(5.0))
                    Text("6x (3072x2688)").tag(Float(6.0))
                    Text("8x (4096x3584)").tag(Float(8.0))
                }
                Text(settings.localized("Lower values can help performance on heavy games. Higher values improve visual quality but reduce performance significantly. Requires restart."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Filtering")) {
                Picker(settings.localized("Texture Filtering"), selection: $settings.textureFiltering) {
                    Text(settings.localized("Nearest (Pixelated)")).tag(0)
                    Text(settings.localized("Bilinear (Forced)")).tag(1)
                    Text(settings.localized("Bilinear (PS2 Default)")).tag(2)
                    Text(settings.localized("Bilinear (Forced excl. Sprite)")).tag(3)
                }

                Toggle(settings.localized("Hardware Mipmapping"), isOn: $settings.hardwareMipmapping)
                Text(settings.localized("Emulates PS2 texture mipmaps in the hardware renderer. Leave on by default; turn off only if a game has mipmap shimmer, stripes, or bad texture LOD behavior. Requires reset/relaunch for safest results."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle("FXAA", isOn: $settings.fxaa)
                Text(settings.localized("Fast anti-aliasing. Smooths edges but may blur textures slightly."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle(settings.localized("CAS Sharpening"), isOn: Binding(
                    get: { settings.casMode > 0 },
                    set: { settings.casMode = $0 ? 1 : 0 }
                ))
                if settings.casMode > 0 {
                    HStack {
                        Text(settings.localized("Sharpness"))
                        Slider(value: Binding(
                            get: { Float(settings.casSharpness) / 100.0 },
                            set: { settings.casSharpness = Int($0 * 100) }
                        ), in: 0...1)
                        Text("\(settings.casSharpness)%")
                            .font(.caption)
                            .frame(width: 40)
                    }
                }
                Text(settings.localized("Contrast Adaptive Sharpening via Metal. Sharpens the image after rendering."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Display")) {
                Picker(settings.localized("Deinterlace"), selection: $settings.interlaceMode) {
                    Text(settings.localized("None")).tag(0)
                    Text(settings.localized("Weave (TFF)")).tag(1)
                    Text(settings.localized("Weave (BFF)")).tag(2)
                    Text(settings.localized("Bob (TFF)")).tag(3)
                    Text(settings.localized("Bob (BFF)")).tag(4)
                    Text(settings.localized("Blend (TFF)")).tag(5)
                    Text(settings.localized("Blend (BFF)")).tag(6)
                    Text(settings.localized("Adaptive (Default)")).tag(7)
                }

                Picker(settings.localized("Aspect Ratio"), selection: $settings.aspectRatio) {
                    Text(settings.localized("Auto 4:3 / 3:2 (Default)")).tag(1)
                    Text("4:3").tag(2)
                    Text(settings.localized("16:9 (Widescreen)")).tag(3)
                    Text("10:7").tag(4)
                    Text(settings.localized("Stretch to Window")).tag(0)
                }
            }

            Section(settings.localized("Quality")) {
                Picker(settings.localized("Blending Accuracy"), selection: $settings.blendingAccuracy) {
                    Text(settings.localized("Minimum (Fast)")).tag(0)
                    Text(settings.localized("Basic (Default)")).tag(1)
                    Text(settings.localized("Medium")).tag(2)
                    Text(settings.localized("High")).tag(3)
                    Text(settings.localized("Full (Slow)")).tag(4)
                    Text(settings.localized("Ultra (Very Slow)")).tag(5)
                }
                Text(settings.localized("Higher accuracy fixes transparency issues but reduces performance."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Picker(settings.localized("Dithering"), selection: $settings.dithering) {
                    Text(settings.localized("Off")).tag(0)
                    Text(settings.localized("Unscaled")).tag(1)
                    Text(settings.localized("Scaled (Default)")).tag(2)
                }
            }

            Section(settings.localized("Texture Replacement")) {
                Toggle(settings.localized("Load Replacement Textures"), isOn: $settings.loadTextureReplacements)
                Text(settings.localized("Loads PNG or DDS texture packs from Documents/textures/[Game Serial]/replacements/. Requires restart."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle(settings.localized("Async Loading"), isOn: $settings.loadTextureReplacementsAsync)
                    .disabled(!settings.loadTextureReplacements)
                Text(settings.localized("Loads replacement textures in the background to reduce boot stalls."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle(settings.localized("Precache Textures"), isOn: $settings.precacheTextureReplacements)
                    .disabled(!settings.loadTextureReplacements)
                Text(settings.localized("Loads all replacements when the game starts. Faster in-game, but uses more RAM."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Picker(settings.localized("Texture Preloading"), selection: $settings.texturePreloading) {
                    Text(settings.localized("Off")).tag(0)
                    Text(settings.localized("Partial")).tag(1)
                    Text(settings.localized("Full")).tag(2)
                }
                Text(settings.localized("Core texture preloading mode. Full can improve replacement behavior but may increase memory use."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section(settings.localized("Texture Dumping")) {
                Toggle(settings.localized("Dump Replaceable Textures"), isOn: $settings.dumpReplaceableTextures)
                Text(settings.localized("Writes discovered textures to Documents/textures/[Game Serial]/dumps/. This can heavily reduce performance."))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Toggle(settings.localized("Dump Mipmaps"), isOn: $settings.dumpReplaceableMipmaps)
                    .disabled(!settings.dumpReplaceableTextures)
                Toggle(settings.localized("Dump During FMV"), isOn: $settings.dumpTexturesWithFMVActive)
                    .disabled(!settings.dumpReplaceableTextures)
                Toggle(settings.localized("Dump Direct Textures"), isOn: $settings.dumpDirectTextures)
                    .disabled(!settings.dumpReplaceableTextures)
                Toggle(settings.localized("Dump Palette Textures"), isOn: $settings.dumpPaletteTextures)
                    .disabled(!settings.dumpReplaceableTextures)
            }

            Section("VSync") {
                Stepper("\(settings.localized("Queue Size")): \(settings.vsyncQueueSize)", value: $settings.vsyncQueueSize, in: 2...16)
                Text(settings.localized("Higher values reduce frame drops but increase latency."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                Button(settings.localized("Reset Graphics to Defaults")) {
                    settings.resetGraphicsDefaults()
                }
                .foregroundStyle(.red)
            }
        }
        .navigationTitle(settings.localized("Graphics"))
        .navigationBarTitleDisplayMode(.inline)
    }
}
