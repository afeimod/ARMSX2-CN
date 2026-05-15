// RootView.swift — Root view switching between menu and game
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct RootView: View {
    @State private var appState = AppState.shared
    @State private var fileImporter = FileImportHandler.shared
    @State private var showBootSplash = true

    var body: some View {
        ZStack {
            switch appState.currentScreen {
            case .menu:
                Color(uiColor: .systemGroupedBackground)
                    .ignoresSafeArea()
                MenuTabView()
            case .playing:
                GameScreenView()
            }

            if showBootSplash {
                BootSplashView {
                    withAnimation(.easeOut(duration: 0.2)) {
                        showBootSplash = false
                    }
                }
                .transition(.opacity)
                .zIndex(100)
            }
        }
        .statusBarHidden(showBootSplash)
        .onOpenURL { url in
            fileImporter.handleURL(url)
        }
        .alert("File Import", isPresented: $fileImporter.showImportAlert) {
            Button("OK") {}
        } message: {
            Text(fileImporter.lastImportMessage ?? "")
        }
    }
}

struct MenuTabView: View {
    @State private var appState = AppState.shared
    @State private var selectedTab = 0

    var body: some View {
#if targetEnvironment(macCatalyst)
        VStack(spacing: 0) {
            CatalystMenuTabBar(selectedTab: $selectedTab)
                .padding(.top, 8)
                .padding(.bottom, 8)

            Group {
                switch selectedTab {
                case 0:
                    GameListView()
                case 1:
                    BIOSListView()
                case 2:
                    HelpView()
                default:
                    SettingsRootView()
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .tint(.blue)
#else
        TabView(selection: $selectedTab) {
            GameListView()
                .tabItem {
                    Label("Games", systemImage: "gamecontroller")
                }
                .tag(0)

            BIOSListView()
                .tabItem {
                    Label("BIOS", systemImage: "cpu")
                }
                .tag(1)

            HelpView()
                .tabItem {
                    Label("Help", systemImage: "questionmark.circle")
                }
                .tag(2)

            NavigationStack {
                SettingsRootView()
            }
            .tabItem {
                Label("Settings", systemImage: "gearshape")
            }
            .tag(3)
        }
        .tint(.blue)
#endif
    }
}

#if targetEnvironment(macCatalyst)
private struct CatalystMenuTabBar: View {
    @Binding var selectedTab: Int

    private let tabs = [
        (0, "Games"),
        (1, "BIOS"),
        (2, "Help"),
        (3, "Settings"),
    ]

    var body: some View {
        HStack(spacing: 2) {
            ForEach(tabs, id: \.0) { tab in
                Button {
                    selectedTab = tab.0
                } label: {
                    Text(tab.1)
                        .font(.callout)
                        .fontWeight(selectedTab == tab.0 ? .semibold : .regular)
                        .foregroundStyle(.primary)
                        .frame(minWidth: 82)
                        .padding(.vertical, 6)
                        .background {
                            if selectedTab == tab.0 {
                                Capsule()
                                    .fill(Color.primary.opacity(0.12))
                            }
                        }
                }
                .buttonStyle(.plain)

                if tab.0 != tabs.last?.0 {
                    Divider()
                        .frame(height: 20)
                }
            }
        }
        .padding(4)
        .background(.regularMaterial, in: Capsule())
        .shadow(color: .black.opacity(0.08), radius: 18, y: 8)
    }
}
#endif
