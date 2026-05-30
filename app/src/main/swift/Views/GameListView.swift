// GameListView.swift — ROM list with favorites
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers
import UIKit

struct ISOEntry: Identifiable {
    var id: String { name }
    let name: String
    let fileURL: URL?
    let coverURL: URL?
    let metadata: [String: String]
    let size: UInt64
    var isFavorite: Bool

    var coverInfo: CoverGameInfo {
        CoverGameInfo(name: name, fileURL: fileURL, metadata: metadata, hasCover: coverURL != nil)
    }
}

struct GameListView: View {
    @State private var games: [ISOEntry] = []
    @State private var appState = AppState.shared
    @State private var fileImporter = FileImportHandler.shared
    @State private var coverStore = CoverStore.shared
    @State private var showGameImporter = false
    @State private var showCoverImporter = false
    @State private var showRestartAlert = false
    @State private var showStopAlert = false
    @State private var showCoverTemplateEditor = false
    @State private var coverTemplateDraft = CoverStore.defaultCoverURLTemplate
    @State private var pendingGameName: String = ""
    @State private var pendingCoverGameName: String?

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
                    Menu {
                        Button {
                            pendingCoverGameName = nil
                            showCoverImporter = true
                        } label: {
                            Label("Import Local Covers", systemImage: "photo.badge.plus")
                        }

                        Button {
                            downloadMissingCovers()
                        } label: {
                            Label("Download Missing Covers", systemImage: "icloud.and.arrow.down")
                        }
                        .disabled(coverStore.isDownloadingCovers || games.isEmpty)

                        Button {
                            coverTemplateDraft = coverStore.coverURLTemplate
                            showCoverTemplateEditor = true
                        } label: {
                            Label("Cover Source", systemImage: "link")
                        }

                        Button {
                            coverStore.coverURLTemplate = CoverStore.defaultCoverURLTemplate
                            coverStore.lastCoverMessage = "Cover URL template reset to the ARMSX2 Android default."
                            coverStore.showCoverAlert = true
                        } label: {
                            Label("Reset Cover Template", systemImage: "arrow.counterclockwise")
                        }
                    } label: {
                        Image(systemName: coverStore.isDownloadingCovers ? "icloud.and.arrow.down" : "photo.stack")
                    }
                    .accessibilityLabel("Covers")
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
            .alert("Import Result", isPresented: $fileImporter.showImportAlert) {
                Button("OK") {}
            } message: {
                Text(fileImporter.lastImportMessage ?? "")
            }
            .alert("Cover Result", isPresented: $coverStore.showCoverAlert) {
                Button("OK") {}
            } message: {
                Text(coverStore.lastCoverMessage ?? "")
            }
            .alert("Cover Source", isPresented: $showCoverTemplateEditor) {
                TextField("https://.../${serial}.jpg", text: $coverTemplateDraft)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button("Cancel", role: .cancel) {}
                Button("Save") {
                    coverStore.coverURLTemplate = coverTemplateDraft
                    if games.isEmpty {
                        coverStore.lastCoverMessage = "Cover URL template saved."
                        coverStore.showCoverAlert = true
                    } else {
                        downloadMissingCovers()
                    }
                }
            } message: {
                Text("Use ${serial}, ${title}, or ${filetitle}. Default: \(CoverStore.defaultCoverURLTemplate)")
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
            .fileImporter(
                isPresented: $showCoverImporter,
                allowedContentTypes: CoverStore.coverContentTypes,
                allowsMultipleSelection: pendingCoverGameName == nil
            ) { result in
                switch result {
                case .success(let urls):
                    coverStore.importCoverURLs(urls, forGameNamed: pendingCoverGameName)
                    pendingCoverGameName = nil
                    loadGames()
                case .failure(let error):
                    coverStore.lastCoverMessage = "Cover import failed: \(error.localizedDescription)"
                    coverStore.showCoverAlert = true
                    pendingCoverGameName = nil
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
            HStack(spacing: 12) {
                coverThumbnail(for: game)

                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 6) {
                        Text(coverStore.displayName(forGameName: game.name))
                            .font(.body)
                            .fontWeight(.medium)
                            .foregroundStyle(.primary)
                        if game.name == appState.runningGameName {
                            Image(systemName: "circle.fill")
                                .font(.system(size: 8))
                                .foregroundStyle(.green)
                        }
                    }
                    HStack(spacing: 8) {
                        Text(formatSize(game.size))
                        Text(game.name.pathExtensionLabel)
                    }
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
        .contextMenu {
            Button {
                downloadCover(for: game)
            } label: {
                Label("Download Cover", systemImage: "icloud.and.arrow.down")
            }
            .disabled(coverStore.isDownloadingCovers)

            Button {
                pendingCoverGameName = game.name
                showCoverImporter = true
            } label: {
                Label("Choose Cover", systemImage: "photo")
            }

            if game.coverURL != nil {
                Button(role: .destructive) {
                    coverStore.removeManagedCovers(forGameNamed: game.name)
                    loadGames()
                } label: {
                    Label("Remove Cover", systemImage: "trash")
                }
            }
        }
    }

    private func coverThumbnail(for game: ISOEntry) -> some View {
        ZStack {
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(.thinMaterial)
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(.white.opacity(0.12), lineWidth: 1)

            if let coverURL = game.coverURL, let image = UIImage(contentsOfFile: coverURL.path) {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
                    .frame(width: 58, height: 87)
                    .clipped()
            } else {
                VStack(spacing: 6) {
                    Image(systemName: game.name.lowercased().hasSuffix(".chd") ? "archivebox" : "opticaldisc")
                        .font(.system(size: 24, weight: .medium))
                    Text(game.name.lowercased().hasSuffix(".chd") ? "CHD" : "PS2")
                        .font(.caption2)
                        .fontWeight(.bold)
                }
                .foregroundStyle(.secondary)
            }
        }
        .frame(width: 58, height: 87)
        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        .shadow(color: .black.opacity(0.12), radius: 6, x: 0, y: 3)
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
            let fileURL = fm.fileExists(atPath: path) ? URL(fileURLWithPath: path) : nil
            let metadata = ARMSX2Bridge.gameMetadata(forISO: name)
            let coverURL = coverStore.coverURL(forGameName: name, gamePath: fileURL, metadata: metadata)
            return ISOEntry(name: name, fileURL: fileURL, coverURL: coverURL, metadata: metadata, size: size, isFavorite: fav)
        }
    }

    private func downloadMissingCovers() {
        let targets = games.map(\.coverInfo)
        Task {
            _ = await coverStore.downloadMissingCovers(for: targets)
            loadGames()
        }
    }

    private func downloadCover(for game: ISOEntry) {
        Task {
            _ = await coverStore.downloadMissingCovers(for: [game.coverInfo])
            loadGames()
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

private extension String {
    var pathExtensionLabel: String {
        let ext = (self as NSString).pathExtension.uppercased()
        return ext.isEmpty ? "FILE" : ext
    }
}
