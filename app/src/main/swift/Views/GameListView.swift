// GameListView.swift — ROM list with favorites
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers
import UIKit
import ImageIO

struct ISOEntry: Identifiable {
    var id: String { name }
    let name: String
    let fileURL: URL?
    let coverURL: URL?
    let coverSignature: String?
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
    @State private var settings = SettingsStore.shared
    @State private var fileImporter = FileImportHandler.shared
    @State private var coverStore = CoverStore.shared
    @State private var showGameImporter = false
    @State private var showCoverImporter = false
    @State private var showRestartAlert = false
    @State private var showStopAlert = false
    @State private var showCoverTemplateEditor = false
    @State private var showPNACHImporter = false
    @State private var coverTemplateDraft = CoverStore.defaultCoverURLTemplate
    @State private var pendingGameName: String = ""
    @State private var pendingCoverGameName: String?
    @State private var pendingPNACHGameName: String?
    @State private var gameInfoTarget: ISOEntry?
    @State private var gameSettingsTarget: ISOEntry?
    @AppStorage("ARMSX2iOSGameLibraryLayout") private var libraryLayout = "grid"

    var body: some View {
        NavigationStack {
            Group {
                if games.isEmpty && appState.runningGameName == nil {
                    emptyState
                } else if libraryLayout == "grid" {
                    gridLibrary
                } else {
                    listLibrary
#if targetEnvironment(macCatalyst)
                    .listStyle(.inset)
#endif
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .navigationTitle(settings.localized("Games"))
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button { showGameImporter = true } label: {
                        Image(systemName: "plus")
                    }
                    .accessibilityLabel(settings.localized("Import Games"))
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        libraryLayout = libraryLayout == "grid" ? "list" : "grid"
                    } label: {
                        Image(systemName: libraryLayout == "grid" ? "list.bullet" : "square.grid.2x2")
                    }
                    .accessibilityLabel(libraryLayout == "grid" ? "Show List" : "Show Grid")
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Button {
                            pendingCoverGameName = nil
                            showCoverImporter = true
                        } label: {
                            Label(settings.localized("Import Local Covers"), systemImage: "photo.badge.plus")
                        }

                        Button {
                            downloadMissingCovers()
                        } label: {
                            Label(settings.localized("Download Missing Covers"), systemImage: "icloud.and.arrow.down")
                        }
                        .disabled(coverStore.isDownloadingCovers || games.isEmpty)

                        Button {
                            coverTemplateDraft = coverStore.coverURLTemplate
                            showCoverTemplateEditor = true
                        } label: {
                            Label(settings.localized("Cover Source"), systemImage: "link")
                        }

                        Button {
                            coverStore.coverURLTemplate = CoverStore.defaultCoverURLTemplate
                            coverStore.lastCoverMessage = "Cover URL template reset to the ARMSX2 Android default."
                            coverStore.showCoverAlert = true
                        } label: {
                            Label(settings.localized("Reset Cover Template"), systemImage: "arrow.counterclockwise")
                        }
                    } label: {
                        Image(systemName: coverStore.isDownloadingCovers ? "icloud.and.arrow.down" : "photo.stack")
                    }
                    .accessibilityLabel(settings.localized("Covers"))
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button { loadGames() } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                }
                ToolbarItem(placement: .topBarLeading) {
                    if ARMSX2Bridge.hasBIOS() {
                        Button(settings.localized("BIOS Only")) {
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
            .alert(settings.localized("Import Result"), isPresented: $fileImporter.showImportAlert) {
                Button(settings.localized("OK")) {}
            } message: {
                Text(fileImporter.lastImportMessage ?? "")
            }
            .alert(settings.localized("Cover Result"), isPresented: $coverStore.showCoverAlert) {
                Button(settings.localized("OK")) {}
            } message: {
                Text(coverStore.lastCoverMessage ?? "")
            }
            .alert(settings.localized("Cover Source"), isPresented: $showCoverTemplateEditor) {
                TextField("https://.../${serial}.jpg", text: $coverTemplateDraft)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button(settings.localized("Cancel"), role: .cancel) {}
                Button(settings.localized("Save")) {
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
            .alert(settings.localized("Restart VM?"), isPresented: $showRestartAlert) {
                Button(settings.localized("Cancel"), role: .cancel) {}
                Button(settings.localized("Restart"), role: .destructive) {
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
            .sheet(isPresented: $showGameImporter) {
                ImportDocumentPicker(
                    allowedContentTypes: FileImportHandler.gameContentTypes,
                    allowsMultipleSelection: true
                ) { result in
                    showGameImporter = false
                    switch result {
                    case .success(let urls):
                        let importedGames = fileImporter.importURLs(urls, preferredDestination: .game)
                        loadGames()
                        autoDownloadCovers(for: importedGames)
                    case .failure(let error):
                        if (error as NSError).code != NSUserCancelledError {
                            fileImporter.lastImportMessage = "Import failed: \(error.localizedDescription)"
                            fileImporter.showImportAlert = true
                        }
                    }
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
            .sheet(isPresented: $showPNACHImporter) {
                ImportDocumentPicker(
                    allowedContentTypes: FileImportHandler.pnachContentTypes,
                    allowsMultipleSelection: true
                ) { result in
                    showPNACHImporter = false
                    switch result {
                    case .success(let urls):
                        if let pendingPNACHGameName {
                            fileImporter.importPNACHURLs(urls, forISO: pendingPNACHGameName, asCheat: true)
                        } else {
                            fileImporter.lastImportMessage = "Pick a game first, then import its PNACH patch."
                            fileImporter.showImportAlert = true
                        }
                        pendingPNACHGameName = nil
                    case .failure(let error):
                        if (error as NSError).code != NSUserCancelledError {
                            fileImporter.lastImportMessage = "PNACH import failed: \(error.localizedDescription)"
                            fileImporter.showImportAlert = true
                        }
                        pendingPNACHGameName = nil
                    }
                }
            }
            .sheet(item: $gameInfoTarget) { game in
                GameInfoPanel(game: game, coverStore: coverStore)
                    .presentationDetents([.medium, .large])
            }
            .sheet(item: $gameSettingsTarget) { game in
                PerGameSettingsPanel(game: game)
                    .presentationDetents([.large])
            }
        }
        .onAppear { loadGames() }
    }

    private var listLibrary: some View {
        List {
            if let gameName = appState.runningGameName {
                vmStatusSection(gameName: gameName)
            }
            ForEach(games) { game in
                gameRow(game)
            }
        }
    }

    private var gridLibrary: some View {
        ScrollView {
            LazyVStack(spacing: 16) {
                if let gameName = appState.runningGameName {
                    vmStatusCard(gameName: gameName)
                        .padding(.horizontal)
                }

                LazyVGrid(columns: [GridItem(.adaptive(minimum: 142), spacing: 14, alignment: .top)], spacing: 18) {
                    ForEach(games) { game in
                        gameGridCard(game)
                    }
                }
                .padding(.horizontal)
                .padding(.bottom, 20)
            }
            .padding(.top, 12)
        }
        .background(Color(.systemGroupedBackground))
        .transaction { transaction in
            transaction.animation = nil
        }
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
                        Text(settings.localized("Now Running"))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        Text(gameName == "BIOS" ? settings.localized("BIOS Only") : gameName)
                            .font(.body)
                            .fontWeight(.semibold)
                    }
                    Spacer()
                    Text(settings.localized("Resume"))
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
                    Label(settings.localized("Stop Emulation"), systemImage: "stop.circle")
                        .font(.subheadline)
                    Spacer()
                }
            }
        }
        .alert(settings.localized("Stop Emulation?"), isPresented: $showStopAlert) {
            Button(settings.localized("Cancel"), role: .cancel) { }
            Button(settings.localized("Stop"), role: .destructive) {
                ARMSX2Bridge.requestVMStop()
                appState.runningGameName = nil
            }
        } message: {
            Text(settings.localized("This will shut down the running game. All unsaved progress will be lost."))
        }
    }

    private func gameRow(_ game: ISOEntry) -> some View {
        Button {
            open(game)
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
            gameContextMenu(for: game)
        }
    }

    private func gameGridCard(_ game: ISOEntry) -> some View {
        Button {
            open(game)
        } label: {
            VStack(alignment: .leading, spacing: 10) {
                ZStack(alignment: .topTrailing) {
                    coverThumbnail(for: game, width: 126, height: 189)
                        .frame(maxWidth: .infinity)

                    Button {
                        toggleFavorite(game.name)
                    } label: {
                        Image(systemName: game.isFavorite ? "star.fill" : "star")
                            .font(.callout.weight(.semibold))
                            .foregroundStyle(game.isFavorite ? .yellow : .white.opacity(0.86))
                            .padding(8)
                            .background(.black.opacity(0.48), in: Circle())
                    }
                    .buttonStyle(.plain)
                    .padding(6)
                }

                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 5) {
                        Text(coverStore.displayName(forGameName: game.name))
                            .font(.subheadline.weight(.semibold))
                            .foregroundStyle(.primary)
                            .lineLimit(2)
                            .multilineTextAlignment(.leading)
                        if game.name == appState.runningGameName {
                            Image(systemName: "circle.fill")
                                .font(.system(size: 7))
                                .foregroundStyle(.green)
                        }
                    }
                    HStack(spacing: 6) {
                        Text(game.name.pathExtensionLabel)
                        Text(formatSize(game.size))
                    }
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .padding(8)
            .frame(maxWidth: .infinity, minHeight: 268, alignment: .top)
            .background(.background, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: 18, style: .continuous)
                    .strokeBorder(game.name == appState.runningGameName ? .green.opacity(0.6) : .white.opacity(0.08), lineWidth: 1)
            }
        }
        .buttonStyle(.plain)
        .contextMenu {
            gameContextMenu(for: game)
        }
    }

    private func coverThumbnail(for game: ISOEntry, width: CGFloat = 58, height: CGFloat = 87) -> some View {
        CoverThumbnailView(
            gameName: game.name,
            coverURL: game.coverURL,
            coverSignature: game.coverSignature,
            width: width,
            height: height
        )
    }

    private func vmStatusCard(gameName: String) -> some View {
        HStack(spacing: 12) {
            Image(systemName: "play.circle.fill")
                .font(.title2)
                .foregroundStyle(.green)
            VStack(alignment: .leading, spacing: 3) {
                Text(settings.localized("Now Running"))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(gameName == "BIOS" ? settings.localized("BIOS Only") : gameName)
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)
            }
            Spacer()
            Button(settings.localized("Resume")) {
                appState.returnToGame()
            }
            .buttonStyle(.borderedProminent)
            Button(role: .destructive) {
                showStopAlert = true
            } label: {
                Image(systemName: "stop.circle")
            }
            .buttonStyle(.bordered)
        }
        .padding()
        .background(.background, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        .alert(settings.localized("Stop Emulation?"), isPresented: $showStopAlert) {
            Button(settings.localized("Cancel"), role: .cancel) { }
            Button(settings.localized("Stop"), role: .destructive) {
                ARMSX2Bridge.requestVMStop()
                appState.runningGameName = nil
            }
        } message: {
            Text(settings.localized("This will shut down the running game. All unsaved progress will be lost."))
        }
    }

    @ViewBuilder
    private func gameContextMenu(for game: ISOEntry) -> some View {
        Button {
            gameInfoTarget = game
        } label: {
            Label(settings.localized("Game Info"), systemImage: "info.circle")
        }

        Button {
            gameSettingsTarget = game
        } label: {
            Label(settings.localized("Per-Game Settings"), systemImage: "slider.horizontal.3")
        }

        Button {
            pendingPNACHGameName = game.name
            showPNACHImporter = true
        } label: {
            Label(settings.localized("Import PNACH / 60 FPS Patch"), systemImage: "wand.and.stars")
        }

        Button {
            downloadCover(for: game)
        } label: {
            Label(settings.localized("Download Cover"), systemImage: "icloud.and.arrow.down")
        }
        .disabled(coverStore.isDownloadingCovers)

        Button {
            pendingCoverGameName = game.name
            showCoverImporter = true
        } label: {
            Label(settings.localized("Choose Cover"), systemImage: "photo")
        }

        if game.coverURL != nil {
            Button(role: .destructive) {
                coverStore.removeManagedCovers(forGameNamed: game.name)
                loadGames()
            } label: {
                Label(settings.localized("Remove Cover"), systemImage: "trash")
            }
        }
    }

    private func open(_ game: ISOEntry) {
        if game.name == appState.runningGameName {
            appState.returnToGame()
        } else if appState.runningGameName != nil {
            pendingGameName = game.name
            showRestartAlert = true
        } else {
            appState.bootGame(isoName: game.name)
        }
    }

    private var emptyState: some View {
        VStack(spacing: 16) {
            Image(systemName: "opticaldisc")
                .font(.system(size: 48))
                .foregroundStyle(.secondary)
            Text(settings.localized("No Games Found"))
                .font(.title2)
                .fontWeight(.semibold)
            Text(settings.localized("Import PS2 disc images to add them here."))
                .font(.body)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Button {
                showGameImporter = true
            } label: {
                Label(settings.localized("Import Games"), systemImage: "plus")
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
            let coverSignature = CoverThumbnailCache.signature(for: coverURL)
            return ISOEntry(name: name, fileURL: fileURL, coverURL: coverURL, coverSignature: coverSignature, metadata: metadata, size: size, isFavorite: fav)
        }.sorted { a, b in
            if a.isFavorite != b.isFavorite { return a.isFavorite }
            return a.name.localizedCaseInsensitiveCompare(b.name) == .orderedAscending
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

    private func autoDownloadCovers(for importedGames: [FileImportHandler.ImportedGame]) {
        guard !importedGames.isEmpty else { return }

        let targets = importedGames.map { game in
            let metadata = ARMSX2Bridge.gameMetadata(forISO: game.name)
            let existingCover = coverStore.coverURL(forGameName: game.name, gamePath: game.fileURL, metadata: metadata)
            return CoverGameInfo(name: game.name, fileURL: game.fileURL, metadata: metadata, hasCover: existingCover != nil)
        }

        Task {
            let summary = await coverStore.downloadMissingCovers(for: targets, showResult: false)
            if summary.downloaded > 0 {
                loadGames()
            }
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

private struct GameInfoPanel: View {
    @Environment(\.dismiss) private var dismiss

    let game: ISOEntry
    let coverStore: CoverStore

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    HStack(spacing: 14) {
                        CoverThumbnailView(
                            gameName: game.name,
                            coverURL: game.coverURL,
                            coverSignature: game.coverSignature,
                            width: 84,
                            height: 126
                        )

                        VStack(alignment: .leading, spacing: 6) {
                            Text(coverStore.displayName(forGameName: game.name))
                                .font(.headline)
                            Text(game.name)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                                .textSelection(.enabled)
                        }
                    }
                    .padding(.vertical, 4)
                }

                Section("Disc") {
                    LabeledContent("Region") {
                        Text(regionDisplay)
                    }
                    LabeledContent("Serial") {
                        Text(metadataValue("serial"))
                            .textSelection(.enabled)
                    }
                    LabeledContent("CRC") {
                        Text(metadataValue("crc"))
                            .textSelection(.enabled)
                    }
                    LabeledContent("Format") {
                        Text(game.name.pathExtensionLabel)
                    }
                    LabeledContent("Size") {
                        Text(formatSize(game.size))
                    }
                }

                Section("File") {
                    Text(game.fileURL?.path ?? "File path unavailable")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                }
            }
            .navigationTitle("Game Info")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") {
                        dismiss()
                    }
                }
            }
        }
    }

    private var regionDisplay: String {
        let region = metadataValue("region")
        return "\(Self.regionFlag(for: region)) \(region)"
    }

    private func metadataValue(_ key: String) -> String {
        let value = game.metadata[key]?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return value.isEmpty ? "Unknown" : value
    }

    private func formatSize(_ bytes: UInt64) -> String {
        let gb = Double(bytes) / 1_073_741_824
        if gb >= 1.0 {
            return String(format: "%.1f GB", gb)
        }
        let mb = Double(bytes) / 1_048_576
        return String(format: "%.0f MB", mb)
    }

    private static func regionFlag(for region: String) -> String {
        let value = region.lowercased()
        if value.contains("japan") || value.contains("ntsc-j") {
            return "🇯🇵"
        }
        if value.contains("usa") || value.contains("america") || value.contains("ntsc-u") {
            return "🇺🇸"
        }
        if value.contains("europe") || value.contains("pal") {
            return "🇪🇺"
        }
        if value.contains("korea") || value.contains("ntsc-k") {
            return "🇰🇷"
        }
        if value.contains("china") || value.contains("ntsc-c") {
            return "🇨🇳"
        }
        if value.contains("hong kong") || value.contains("ntsc-hk") {
            return "🇭🇰"
        }
        if value.contains("australia") {
            return "🇦🇺"
        }
        return "🌐"
    }
}

struct PerGameSettingsPanel: View {
    @Environment(\.dismiss) private var dismiss

    let game: ISOEntry
    let onDone: (() -> Void)?

    @State private var enabled: Bool
    @State private var upscaleMultiplier: Float
    @State private var aspectRatio: String
    @State private var textureFiltering: Int
    @State private var hardwareMipmapping: Bool
    @State private var blendingAccuracy: Int
    @State private var enableCheats: Bool
    @State private var enablePatches: Bool
    @State private var enableGameFixes: Bool
    @State private var enableGameDBHardwareFixes: Bool
    @State private var statusMessage: String?

    init(game: ISOEntry, onDone: (() -> Void)? = nil) {
        self.game = game
        self.onDone = onDone
        let info = ARMSX2Bridge.gameSettings(forISO: game.name)
        _enabled = State(initialValue: Self.boolValue(info["enabled"], defaultValue: false))
        _upscaleMultiplier = State(initialValue: Self.floatValue(info["upscaleMultiplier"], defaultValue: 1.0))
        _aspectRatio = State(initialValue: Self.normalizedAspect(info["aspectRatio"] as? String))
        _textureFiltering = State(initialValue: Self.intValue(info["textureFiltering"], defaultValue: 2))
        _hardwareMipmapping = State(initialValue: Self.boolValue(info["hardwareMipmapping"], defaultValue: true))
        _blendingAccuracy = State(initialValue: Self.intValue(info["blendingAccuracy"], defaultValue: 1))
        _enableCheats = State(initialValue: Self.boolValue(info["enableCheats"], defaultValue: false))
        _enablePatches = State(initialValue: Self.boolValue(info["enablePatches"], defaultValue: true))
        _enableGameFixes = State(initialValue: Self.boolValue(info["enableGameFixes"], defaultValue: true))
        _enableGameDBHardwareFixes = State(initialValue: Self.boolValue(info["enableGameDBHardwareFixes"], defaultValue: true))
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Toggle("Use Per-Game Overrides", isOn: $enabled)
                    Text("Overrides are saved for this game only and apply on the next boot/reset of this title.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Section("Graphics") {
                    Picker("Internal Resolution", selection: $upscaleMultiplier) {
                        Text("0.25x (Fastest)").tag(Float(0.25))
                        Text("0.5x").tag(Float(0.5))
                        Text("0.75x").tag(Float(0.75))
                        Text("1x Native").tag(Float(1.0))
                        Text("2x").tag(Float(2.0))
                        Text("3x").tag(Float(3.0))
                        Text("4x").tag(Float(4.0))
                    }
                    .disabled(!enabled)

                    Picker("Aspect Ratio", selection: $aspectRatio) {
                        Text("Auto 4:3 / 3:2").tag("Auto 4:3/3:2")
                        Text("4:3").tag("4:3")
                        Text("16:9").tag("16:9")
                        Text("10:7").tag("10:7")
                        Text("Stretch").tag("Stretch")
                    }
                    .disabled(!enabled)

                    Picker("Texture Filtering", selection: $textureFiltering) {
                        Text("Nearest").tag(0)
                        Text("Bilinear Forced").tag(1)
                        Text("Bilinear PS2 Default").tag(2)
                        Text("Bilinear excl. Sprite").tag(3)
                    }
                    .disabled(!enabled)

                    Toggle("Hardware Mipmapping", isOn: $hardwareMipmapping)
                        .disabled(!enabled)
                    Text("Turn this off only for games with mipmap-related texture stripes, shimmer, or bad LOD. Reset/relaunch the game after changing it.")
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    Picker("Blending Accuracy", selection: $blendingAccuracy) {
                        Text("Minimum").tag(0)
                        Text("Basic").tag(1)
                        Text("Medium").tag(2)
                        Text("High").tag(3)
                        Text("Full").tag(4)
                        Text("Ultra").tag(5)
                    }
                    .disabled(!enabled)
                }

                Section("Patches & Cheats") {
                    Toggle("Enable PNACH Cheats", isOn: $enableCheats)
                        .disabled(!enabled)
                    Toggle("GameDB PNACH Patches", isOn: $enablePatches)
                        .disabled(!enabled)
                    Toggle("GameDB Core Fixes", isOn: $enableGameFixes)
                        .disabled(!enabled)
                    Toggle("GameDB Graphics Fixes", isOn: $enableGameDBHardwareFixes)
                        .disabled(!enabled)
                    Text("If a game looks worse after GameDB, turn off GameDB Graphics Fixes for this game and reset/relaunch it. Core fixes cover timing, clamps, and other compatibility behavior.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                if let statusMessage {
                    Section {
                        Text(statusMessage)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .navigationTitle("Per-Game Settings")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") {
                        if let onDone {
                            onDone()
                        } else {
                            dismiss()
                        }
                    }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        save()
                    }
                }
            }
        }
    }

    private func save() {
        ARMSX2Bridge.setGameSettings(
            forISO: game.name,
            enabled: enabled,
            upscaleMultiplier: upscaleMultiplier,
            aspectRatio: aspectRatio,
            textureFiltering: Int32(textureFiltering),
            hardwareMipmapping: hardwareMipmapping,
            blendingAccuracy: Int32(blendingAccuracy),
            enableCheats: enableCheats,
            enablePatches: enablePatches,
            enableGameFixes: enableGameFixes,
            enableGameDBHardwareFixes: enableGameDBHardwareFixes
        )
        statusMessage = enabled ? "Saved for \(game.metadata["serial"] ?? game.name). Reset or relaunch the game to apply." : "Per-game overrides cleared."
    }

    private static func normalizedAspect(_ value: String?) -> String {
        switch value {
        case "Stretch", "4:3", "16:9", "10:7":
            return value ?? "Auto 4:3/3:2"
        default:
            return "Auto 4:3/3:2"
        }
    }

    private static func boolValue(_ value: Any?, defaultValue: Bool) -> Bool {
        if let bool = value as? Bool {
            return bool
        }
        if let number = value as? NSNumber {
            return number.boolValue
        }
        return defaultValue
    }

    private static func intValue(_ value: Any?, defaultValue: Int) -> Int {
        if let number = value as? NSNumber {
            return number.intValue
        }
        return defaultValue
    }

    private static func floatValue(_ value: Any?, defaultValue: Float) -> Float {
        if let number = value as? NSNumber {
            return number.floatValue
        }
        return defaultValue
    }
}

private struct CoverThumbnailView: View {
    let gameName: String
    let coverURL: URL?
    let coverSignature: String?
    let width: CGFloat
    let height: CGFloat

    @State private var image: UIImage?

    private var cacheID: String {
        "\(coverSignature ?? "placeholder")|\(Int(width))x\(Int(height))"
    }

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .fill(Color(.secondarySystemGroupedBackground))
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(.white.opacity(0.12), lineWidth: 1)

            if let image {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
                    .frame(width: width, height: height)
                    .clipped()
            } else {
                VStack(spacing: 6) {
                    Image(systemName: gameName.lowercased().hasSuffix(".chd") ? "archivebox" : "opticaldisc")
                        .font(.system(size: 24, weight: .medium))
                    Text(gameName.lowercased().hasSuffix(".chd") ? "CHD" : "PS2")
                        .font(.caption2)
                        .fontWeight(.bold)
                }
                .foregroundStyle(.secondary)
            }
        }
        .frame(width: width, height: height)
        .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        .task(id: cacheID) {
            await loadThumbnail()
        }
    }

    @MainActor
    private func loadThumbnail() async {
        guard let coverURL else {
            image = nil
            return
        }

        let scale = UIScreen.main.scale
        if let cached = CoverThumbnailCache.shared.cachedImage(for: coverURL, signature: coverSignature, width: width, height: height, scale: scale) {
            image = cached
            return
        }

        image = await CoverThumbnailCache.shared.thumbnail(for: coverURL, signature: coverSignature, width: width, height: height, scale: scale)
    }
}

private final class CoverThumbnailCache: @unchecked Sendable {
    static let shared = CoverThumbnailCache()

    private let cache = NSCache<NSString, UIImage>()

    private init() {
        cache.countLimit = 768
        cache.totalCostLimit = 96 * 1024 * 1024
    }

    func cachedImage(for url: URL, signature: String?, width: CGFloat, height: CGFloat, scale: CGFloat) -> UIImage? {
        cache.object(forKey: cacheKey(for: url, signature: signature, width: width, height: height, scale: scale))
    }

    func thumbnail(for url: URL, signature: String?, width: CGFloat, height: CGFloat, scale: CGFloat) async -> UIImage? {
        let key = cacheKey(for: url, signature: signature, width: width, height: height, scale: scale)
        if let cached = cache.object(forKey: key) {
            return cached
        }

        let path = url.path
        let maxPixelSize = max(1, Int(max(width, height) * scale))
        let image = await Task.detached(priority: .utility) {
            let sourceOptions = [kCGImageSourceShouldCache: false] as CFDictionary
            let thumbnailOptions = [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceShouldCacheImmediately: true,
                kCGImageSourceThumbnailMaxPixelSize: maxPixelSize,
            ] as CFDictionary

            guard let source = CGImageSourceCreateWithURL(url as CFURL, sourceOptions),
                  let cgImage = CGImageSourceCreateThumbnailAtIndex(source, 0, thumbnailOptions) else {
                return UIImage(contentsOfFile: path)
            }

            return UIImage(cgImage: cgImage, scale: scale, orientation: .up)
        }.value

        if let image {
            let cost = max(1, Int(image.size.width * image.scale * image.size.height * image.scale * 4))
            cache.setObject(image, forKey: key, cost: cost)
        }

        return image
    }

    static func signature(for url: URL?) -> String? {
        guard let url else { return nil }
        let values = try? url.resourceValues(forKeys: [.contentModificationDateKey, .fileSizeKey])
        let modified = values?.contentModificationDate?.timeIntervalSince1970 ?? 0
        let size = values?.fileSize ?? 0
        return "\(url.path)|\(modified)|\(size)"
    }

    private func cacheKey(for url: URL, signature: String?, width: CGFloat, height: CGFloat, scale: CGFloat) -> NSString {
        let pixelWidth = Int(width * scale)
        let pixelHeight = Int(height * scale)
        return "\(signature ?? url.path)|\(pixelWidth)x\(pixelHeight)" as NSString
    }
}

private extension String {
    var pathExtensionLabel: String {
        let ext = (self as NSString).pathExtension.uppercased()
        return ext.isEmpty ? "FILE" : ext
    }
}
