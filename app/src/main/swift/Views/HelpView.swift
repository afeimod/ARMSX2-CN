// HelpView.swift — Practical user guide
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct HelpSection: Identifiable {
    let id = UUID()
    let title: String
    let icon: String
    let items: [HelpItem]
}

struct HelpItem: Identifiable {
    let id = UUID()
    let question: String
    let answer: String
}

private enum HelpTopic: Hashable, Identifiable {
    case item(section: Int, item: Int)
    case about

    var id: String {
        switch self {
        case .item(let section, let item):
            return "\(section)-\(item)"
        case .about:
            return "about"
        }
    }
}

private let helpData: [HelpSection] = [
    HelpSection(title: "Settings Guide", icon: "gearshape", items: [
        HelpItem(
            question: "EE / IOP / VU0 / VU1 (JIT vs Interpreter)",
            answer: "JIT (Recompiler) is much faster. Interpreter is slower but more stable. If a game crashes or behaves incorrectly, try switching individual components to Interpreter. Changes take effect on next boot."
        ),
        HelpItem(
            question: "Fast Boot",
            answer: "Skips the PS2 BIOS intro and boots the game directly. Some games require this OFF to initialize correctly (e.g. missing 3D graphics)."
        ),
        HelpItem(
            question: "Fastmem",
            answer: "Speeds up EE memory access via direct mapping. Disable if 3D graphics are broken. Requires app restart."
        ),
        HelpItem(
            question: "MTVU",
            answer: "Offloads VU1 processing to a separate thread. Improves performance on multi-core devices, but may cause instability in some games."
        ),
        HelpItem(
            question: "Internal Resolution",
            answer: "1x = native PS2 resolution (recommended). 2x/3x provide higher resolution output but significantly increase GPU load."
        ),
        HelpItem(
            question: "VSync Queue Size",
            answer: "Number of pre-rendered frames. Higher values reduce frame drops but increase input latency. Default: 8."
        ),
    ]),
    HelpSection(title: "Overlay", icon: "speedometer", items: [
        HelpItem(
            question: "Overlay presets",
            answer: "OFF hides the overlay. Simple shows FPS, speed, CPU, and indicators. Detail adds VPS, GPU, and resolution. Full enables the Android-style diagnostic set including GS stats, settings, inputs, frame times, version, and hardware info."
        ),
        HelpItem(
            question: "In-game toggle",
            answer: "Tap the menu button (top-right) during gameplay and select Show/Hide Overlay. This toggles visibility without changing your preset."
        ),
    ]),
    HelpSection(title: "Supported Formats", icon: "doc.circle", items: [
        HelpItem(question: "Game formats", answer: "ISO, CHD, IMG, BIN, CSO, ZSO, GZ, ELF"),
        HelpItem(question: "BIOS formats", answer: "BIN, ROM (dumped from your own PS2)"),
        HelpItem(
            question: "Game covers",
            answer: "Open the Covers menu in Games to import local images, download missing covers, or edit the online Cover Source template. Templates support ${serial}, ${title}, and ${filetitle}; the default example is https://raw.githubusercontent.com/xlenore/ps2-covers/main/covers/default/${serial}.jpg. ARMSX2 iOS scans Documents/armsx2_covers, Documents/covers, the game folder, and Documents for JPG, PNG, WebP, HEIC, or HEIF images named after the game file, file stem, game title, or serial-like names such as SLUS-20312. This also works for CHD games when metadata can be resolved."
        ),
    ]),
]

struct HelpView: View {
#if targetEnvironment(macCatalyst)
    @State private var selectedTopic: HelpTopic? = .item(section: 0, item: 0)
#endif

    var body: some View {
#if targetEnvironment(macCatalyst)
        NavigationSplitView {
            List(selection: $selectedTopic) {
                ForEach(helpData.indices, id: \.self) { sectionIndex in
                    let section = helpData[sectionIndex]
                    Section {
                        ForEach(section.items.indices, id: \.self) { itemIndex in
                            let item = section.items[itemIndex]
                            Text(item.question)
                                .tag(HelpTopic.item(section: sectionIndex, item: itemIndex))
                        }
                    } header: {
                        Label(section.title, systemImage: section.icon)
                    }
                }

                Section {
                    Label("Version", systemImage: "info.circle")
                        .tag(HelpTopic.about)
                } header: {
                    Text("About")
                }
            }
            .navigationTitle("Help")
            .listStyle(.sidebar)
        } detail: {
            helpDetail(for: selectedTopic)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .navigationSplitViewStyle(.balanced)
#else
        NavigationStack {
            List {
                ForEach(helpData) { section in
                    Section {
                        ForEach(section.items) { item in
                            DisclosureGroup {
                                Text(item.answer)
                                    .font(.body)
                                    .foregroundStyle(.secondary)
                                    .padding(.vertical, 4)
                            } label: {
                                Text(item.question)
                                    .font(.body)
                            }
                        }
                    } header: {
                        Label(section.title, systemImage: section.icon)
                    }
                }

                Section {
                    HStack {
                        Text("Version")
                        Spacer()
                        Text(ARMSX2Bridge.buildVersion())
                            .foregroundStyle(.secondary)
                            .font(.caption)
                    }
                } header: {
                    Label("About", systemImage: "info.circle")
                }
            }
            .navigationTitle("Help")
        }
#endif
    }

    @ViewBuilder
    private func helpDetail(for topic: HelpTopic?) -> some View {
        switch topic {
        case .item(let sectionIndex, let itemIndex):
            let section = helpData[sectionIndex]
            let item = section.items[itemIndex]

            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    Label(section.title, systemImage: section.icon)
                        .font(.headline)
                        .foregroundStyle(.secondary)
                    Text(item.question)
                        .font(.largeTitle)
                        .fontWeight(.bold)
                    Text(item.answer)
                        .font(.body)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding(32)
                .frame(maxWidth: .infinity, alignment: .topLeading)
            }
            .navigationTitle(item.question)

        case .about:
            Form {
                Section("App") {
                    HStack {
                        Text("Version")
                        Spacer()
                        Text(ARMSX2Bridge.buildVersion())
                            .foregroundStyle(.secondary)
                            .font(.caption)
                    }
                }
            }
            .navigationTitle("About")

        case .none:
            VStack(spacing: 12) {
                Image(systemName: "questionmark.circle")
                    .font(.system(size: 42))
                    .foregroundStyle(.secondary)
                Text("Select a help topic")
                    .font(.title2)
                    .fontWeight(.semibold)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }
}
