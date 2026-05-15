// GameListView.swift — ROM list with favorites
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers

struct ISOEntry: Identifiable {
    let id = UUID()
    let name: String
    let size: UInt64
    var isFavorite: Bool
}

struct GameListView: View {
    @State private var games: [ISOEntry] = []
    @State private var appState = AppState.shared
    @State private var fileImporter = FileImportHandler.shared
    @State private var showGameImporter = false
    @State private var showRestartAlert = false
    @State private var showStopAlert = false
    @State private var pendingGameName: String = ""

    var sortedGames: [ISOEntry] {
        games.sorted { a, b in
            if a.isFavorite != b.isFavorite { return a.isFavorite }
            return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
        }
    }

    var body: some View {
        NavigationStack {
            Group {
                if games.isEmpty && appState.runningGameName == nil {
                    emptyState
                } else {
                    List {
                        if let gameName = appState.runningGameName {
                            vmStatusSection(gameName: gameName)
                        }
                        ForEach(sortedGames) { game in
                            gameRow(game)
                        }
                    }
#if targetEnvironment(macCatalyst)
                    .listStyle(.inset)
#endif
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .navigationTitle("Games")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button { showGameImporter = true } label: {
                        Image(systemName: "plus")
                    }
                    .accessibilityLabel("Import Games")
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button { loadGames() } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                }
                ToolbarItem(placement: .topBarLeading) {
                    if ARMSX2Bridge.hasBIOS() {
                        Button("BIOS Only") {
                            if appState.runningGameName == "BIOS" {
                                appState.returnToGame()
                            } else if appState.runningGameName != nil {
                                pendingGameName = ""
                                showRestartAlert = true
                            } else {
                                appState.bootBIOSOnly()
                            }
                        }
                        .font(.caption)
                    }
                }
            }
            .alert("Restart VM?", isPresented: $showRestartAlert) {
                Button("Cancel", role: .cancel) {}
                Button("Restart", role: .destructive) {
                    if pendingGameName.isEmpty {
                        appState.shutdownAndBootBIOS()
                    } else {
                        appState.shutdownAndBoot(isoName: pendingGameName)
                    }
                }
            } message: {
                let target = pendingGameName.isEmpty ? "BIOS Only" : pendingGameName
                Text("VM is currently running.\nShut down and start \(target)?")
            }
            .fileImporter(
                isPresented: $showGameImporter,
                allowedContentTypes: FileImportHandler.gameContentTypes,
                allowsMultipleSelection: true
            ) { result in
                switch result {
                case .success(let urls):
                    fileImporter.handleURLs(urls, preferredDestination: .game)
                    loadGames()
                case .failure(let error):
                    fileImporter.lastImportMessage = "Import failed: \(error.localizedDescription)"
                    fileImporter.showImportAlert = true
                }
            }
        }
        .onAppear { loadGames() }
    }

    private func vmStatusSection(gameName: String) -> some View {
        Section {
            // Resume row — tap anywhere to return to game
            Button {
                appState.returnToGame()
            } label: {
                HStack {
                    Image(systemName: "play.circle.fill")
                        .foregroundStyle(.green)
                        .font(.title)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Now Running")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Text(gameName == "BIOS" ? "BIOS Only" : gameName)
                            .font(.body)
                            .fontWeight(.semibold)
                    }
                    Spacer()
                    Text("Resume")
                        .font(.subheadline)
                        .fontWeight(.medium)
                    Image(systemName: "chevron.right")
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                }
                .padding(.vertical, 6)
            }
            .tint(.primary)

            // Stop button — separate row with confirmation alert
            Button(role: .destructive) {
                showStopAlert = true
            } label: {
                HStack {
                    Spacer()
                    Label("Stop Emulation", systemImage: "stop.circle")
                        .font(.subheadline)
                    Spacer()
                }
            }
        }
        .alert("Stop Emulation?", isPresented: $showStopAlert) {
            Button("Cancel", role: .cancel) { }
            Button("Stop", role: .destructive) {
                ARMSX2Bridge.requestVMStop()
                appState.runningGameName = nil
            }
        } message: {
            Text("This will shut down the running game. All unsaved progress will be lost.")
        }
    }

    private func gameRow(_ game: ISOEntry) -> some View {
        Button {
            if game.name == appState.runningGameName {
                appState.returnToGame()
            } else if appState.runningGameName != nil {
                pendingGameName = game.name
                showRestartAlert = true
            } else {
                appState.bootGame(isoName: game.name)
            }
        } label: {
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 6) {
                        Text(game.name)
                            .font(.body)
                            .foregroundStyle(.primary)
                        if game.name == appState.runningGameName {
                            Image(systemName: "circle.fill")
                                .font(.system(size: 8))
                                .foregroundStyle(.green)
                        }
                    }
                    Text(formatSize(game.size))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Button {
                    toggleFavorite(game.name)
                } label: {
                    Image(systemName: game.isFavorite ? "star.fill" : "star")
                        .foregroundStyle(game.isFavorite ? .yellow : .gray)
                }
                .buttonStyle(.plain)

                Image(systemName: game.name == appState.runningGameName ? "play.fill" : "chevron.right")
                    .foregroundStyle(game.name == appState.runningGameName ? .green : .secondary)
                    .font(.caption)
            }
        }
        .foregroundStyle(.primary)
    }

    private var emptyState: some View {
        VStack(spacing: 16) {
            Image(systemName: "opticaldisc")
                .font(.system(size: 48))
                .foregroundStyle(.secondary)
            Text("No Games Found")
                .font(.title2)
                .fontWeight(.semibold)
            Text("Import PS2 disc images to add them here.")
                .font(.body)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Button {
                showGameImporter = true
            } label: {
                Label("Import Games", systemImage: "plus")
            }
            .buttonStyle(.borderedProminent)
        }
        .padding()
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func loadGames() {
        let isoDir = ARMSX2Bridge.isoDirectory()
        let docsDir = ARMSX2Bridge.documentsDirectory()
        let fileNames = ARMSX2Bridge.availableISOs()
        let fm = FileManager.default
        games = fileNames.map { name in
            var path = (isoDir as NSString).appendingPathComponent(name)
            if !fm.fileExists(atPath: path) {
                path = (docsDir as NSString).appendingPathComponent(name)
            }
            let attrs = try? fm.attributesOfItem(atPath: path)
            let size = attrs?[.size] as? UInt64 ?? 0
            let fav = ARMSX2Bridge.isFavorite(name)
            return ISOEntry(name: name, size: size, isFavorite: fav)
        }
    }

    private func toggleFavorite(_ name: String) {
        let current = ARMSX2Bridge.isFavorite(name)
        ARMSX2Bridge.setFavorite(name, favorite: !current)
        loadGames()
    }

    private func formatSize(_ bytes: UInt64) -> String {
        let gb = Double(bytes) / 1_073_741_824
        if gb >= 1.0 {
            return String(format: "%.1f GB", gb)
        }
        let mb = Double(bytes) / 1_048_576
        return String(format: "%.0f MB", mb)
    }
}
