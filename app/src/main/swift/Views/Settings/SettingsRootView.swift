// SettingsRootView.swift — Settings navigation root
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private enum SettingsPane: String, CaseIterable, Identifiable {
    case emulator
    case graphics
    case memoryCards
    case overlay
    case gameController
    case virtualPad
    case licenses
    case about

    var id: String { rawValue }

    var title: String {
        switch self {
        case .emulator:
            return "Emulator"
        case .graphics:
            return "Graphics"
        case .memoryCards:
            return "Memory Cards"
        case .overlay:
            return "Overlay (OSD)"
        case .gameController:
            return "Game Controller"
        case .virtualPad:
            return "Virtual Pad"
        case .licenses:
            return "Licenses & Credits"
        case .about:
            return "About"
        }
    }

    var icon: String {
        switch self {
        case .emulator:
            return "cpu"
        case .graphics:
            return "paintbrush"
        case .memoryCards:
            return "memorychip"
        case .overlay:
            return "text.below.photo"
        case .gameController:
            return "gamecontroller"
        case .virtualPad:
            return "hand.draw"
        case .licenses:
            return "doc.text"
        case .about:
            return "info.circle"
        }
    }
}

struct SettingsRootView: View {
#if targetEnvironment(macCatalyst)
    @State private var selectedPane: SettingsPane? = .emulator
#endif

    var body: some View {
#if targetEnvironment(macCatalyst)
        NavigationSplitView {
            List(SettingsPane.allCases, selection: $selectedPane) { pane in
                Label(pane.title, systemImage: pane.icon)
                    .tag(pane)
            }
            .navigationTitle("Settings")
            .listStyle(.sidebar)
        } detail: {
            settingsDetail(for: selectedPane)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
        .navigationSplitViewStyle(.balanced)
#else
        List {
            Section {
                NavigationLink {
                    EmulatorSettingsView()
                } label: {
                    Label("Emulator", systemImage: "cpu")
                }
                NavigationLink {
                    GraphicsSettingsView()
                } label: {
                    Label("Graphics", systemImage: "paintbrush")
                }
                NavigationLink {
                    MemoryCardSettingsView()
                } label: {
                    Label("Memory Cards", systemImage: "memorychip")
                }
                NavigationLink {
                    OverlaySettingsView()
                } label: {
                    Label("Overlay (OSD)", systemImage: "text.below.photo")
                }
                NavigationLink {
                    GamepadSettingsView()
                } label: {
                    Label("Game Controller", systemImage: "gamecontroller")
                }
                NavigationLink {
                    VirtualPadSettingsView()
                } label: {
                    Label("Virtual Pad", systemImage: "hand.draw")
                }
            }

            Section {
                NavigationLink {
                    LicenseView()
                } label: {
                    Label("Licenses & Credits", systemImage: "doc.text")
                }
            }

            Section("About") {
                HStack {
                    Text("Version")
                    Spacer()
                    Text(ARMSX2Bridge.buildVersion())
                        .foregroundStyle(.secondary)
                        .font(.caption)
                }
            }
        }
        .navigationTitle("Settings")
        .navigationBarTitleDisplayMode(.inline)
#endif
    }

    @ViewBuilder
    private func settingsDetail(for pane: SettingsPane?) -> some View {
        switch pane {
        case .emulator:
            EmulatorSettingsView()
        case .graphics:
            GraphicsSettingsView()
        case .memoryCards:
            MemoryCardSettingsView()
        case .overlay:
            OverlaySettingsView()
        case .gameController:
            GamepadSettingsView()
        case .virtualPad:
            VirtualPadSettingsView()
        case .licenses:
            LicenseView()
        case .about:
            SettingsAboutView()
        case .none:
            VStack(spacing: 12) {
                Image(systemName: "gearshape")
                    .font(.system(size: 42))
                    .foregroundStyle(.secondary)
                Text("Select a setting")
                    .font(.title2)
                    .fontWeight(.semibold)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }
}

private struct SettingsAboutView: View {
    var body: some View {
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
    }
}
