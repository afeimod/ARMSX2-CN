// SettingsRootView.swift — Settings navigation root
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

private enum SettingsPane: String, CaseIterable, Identifiable {
    case emulator
    case graphics
    case network
    case memoryCards
    case retroAchievements
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
        case .network:
            return "Network"
        case .memoryCards:
            return "Memory Cards"
        case .retroAchievements:
            return "RetroAchievements"
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
        case .network:
            return "network"
        case .memoryCards:
            return "memorychip"
        case .retroAchievements:
            return "trophy"
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
                    NetworkSettingsView()
                } label: {
                    Label("Network", systemImage: "network")
                }
                NavigationLink {
                    MemoryCardSettingsView()
                } label: {
                    Label("Memory Cards", systemImage: "memorychip")
                }
                NavigationLink {
                    RetroAchievementsSettingsView()
                } label: {
                    Label("RetroAchievements", systemImage: "trophy")
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
        case .network:
            NetworkSettingsView()
        case .memoryCards:
            MemoryCardSettingsView()
        case .retroAchievements:
            RetroAchievementsSettingsView()
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

private struct NetworkSettingsView: View {
    @State private var settings = SettingsStore.shared

    var body: some View {
        Form {
            Section("PS2 HDD") {
                Toggle("Enable DEV9 Virtual HDD", isOn: $settings.dev9HddEnabled)

                HStack {
                    Text("Image")
                    Spacer()
                    Text(settings.dev9HddFile.isEmpty ? "DEV9hdd.raw" : settings.dev9HddFile)
                        .foregroundStyle(.secondary)
                        .font(.caption)
                }

                Button("Use Default HDD Image") {
                    settings.dev9HddFile = "DEV9hdd.raw"
                }

                Text("Matches ARMSX2 Android's DEV9 HDD option. Requires a VM restart.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Online / Ethernet") {
                Toggle("Enable DEV9 Ethernet", isOn: $settings.dev9EthernetEnabled)

                HStack {
                    Text("Mode")
                    Spacer()
                    Text("Sockets")
                        .foregroundStyle(.secondary)
                }

                Picker("Adapter", selection: $settings.dev9EthDevice) {
                    ForEach(ARMSX2Bridge.dev9NetworkAdapters(), id: \.self) { adapter in
                        Text(adapter).tag(adapter)
                    }
                }

                Toggle("Intercept DHCP", isOn: $settings.dev9InterceptDHCP)
                Toggle("Log DHCP", isOn: $settings.dev9EthLogDHCP)
                Toggle("Log DNS", isOn: $settings.dev9EthLogDNS)

                Text("Sockets is the iOS-safe DEV9 Ethernet mode exposed here. PCAP bridged/switched modes are compiled out of this iOS build, so they are intentionally not selectable until a real iOS backend exists.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Tester Notes") {
                Text("Games still need their in-game PS2 network setup. After changing DEV9 settings, use Reset ROM or restart the VM before testing.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .navigationTitle("Network")
    }
}
