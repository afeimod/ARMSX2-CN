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
    @State private var gameCompatibilityTarget: ISOEntry?
    @State private var pendingDeleteGame: ISOEntry?
    @State private var pendingDeleteDataGame: ISOEntry?
    @State private var gameActionTitle = ""
    @State private var gameActionMessage: String?
    @AppStorage("ARMSX2iOSGameLibraryLayout") private var libraryLayout = "grid"
    @AppStorage("ARMSX2iOSLandscapeCoverFlowEnabled") private var landscapeCoverFlowEnabled = true

    var body: some View {
        NavigationStack {
            GeometryReader { geo in
                Group {
                    if games.isEmpty && appState.runningGameName == nil {
                        emptyState
                    } else if libraryLayout == "grid" && geo.size.width > geo.size.height && landscapeCoverFlowEnabled {
                        coverFlowLibrary
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
            }
            .navigationTitle(settings.localized("Games"))
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button { showGameImporter = true } label: {
                        Image(systemName: "plus")
                    }
                    .accessibilityLabel(settings.localized("Import Games"))
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Button {
                            libraryLayout = libraryLayout == "grid" ? "list" : "grid"
                        } label: {
                            Label(
                                settings.localized(libraryLayout == "grid" ? "Show List" : "Show Grid"),
                                systemImage: libraryLayout == "grid" ? "list.bullet" : "square.grid.2x2"
                            )
                        }

                        if libraryLayout == "grid" {
                            Toggle(isOn: $landscapeCoverFlowEnabled) {
                                Label(settings.localized("Landscape Cover Flow"), systemImage: "rectangle.landscape.rotate")
                            }
                        }
                    } label: {
                        Image(systemName: libraryLayout == "grid" ? "list.bullet" : "square.grid.2x2")
                    }
                    .accessibilityLabel(settings.localized("Library Layout"))
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
                Text("\(settings.localized("VM is currently running."))\n\(settings.localized("Shut down and start")) \(settings.localized(target))?")
            }
            .alert(
                settings.localized("Delete Game Data?"),
                isPresented: Binding(
                    get: { pendingDeleteDataGame != nil },
                    set: { if !$0 { pendingDeleteDataGame = nil } }
                )
            ) {
                Button(settings.localized("Cancel"), role: .cancel) {
                    pendingDeleteDataGame = nil
                }
                Button(settings.localized("Delete Game Data"), role: .destructive) {
                    if let game = pendingDeleteDataGame {
                        deleteGameData(game)
                    }
                    pendingDeleteDataGame = nil
                }
            } message: {
                Text(settings.localized("This clears save states, PNACH files, per-game settings, compatibility overrides, and generated cache for this game. Memory card contents are not deleted."))
            }
            .alert(
                settings.localized("Delete Game?"),
                isPresented: Binding(
                    get: { pendingDeleteGame != nil },
                    set: { if !$0 { pendingDeleteGame = nil } }
                )
            ) {
                Button(settings.localized("Cancel"), role: .cancel) {
                    pendingDeleteGame = nil
                }
                Button(settings.localized("Delete ROM"), role: .destructive) {
                    if let game = pendingDeleteGame {
                        deleteGame(game, deleteData: false)
                    }
                    pendingDeleteGame = nil
                }
                Button(settings.localized("Delete ROM + Game Data"), role: .destructive) {
                    if let game = pendingDeleteGame {
                        deleteGame(game, deleteData: true)
                    }
                    pendingDeleteGame = nil
                }
            } message: {
                Text(settings.localized("Delete the selected game file? You can also remove its generated game data at the same time."))
            }
            .alert(
                settings.localized(gameActionTitle.isEmpty ? "Game Action" : gameActionTitle),
                isPresented: Binding(
                    get: { gameActionMessage != nil },
                    set: { if !$0 { gameActionMessage = nil } }
                )
            ) {
                Button(settings.localized("OK")) {
                    gameActionMessage = nil
                }
            } message: {
                Text(gameActionMessage ?? "")
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
                        if !FileImportHandler.isUserCancelledPickerError(error) {
                            fileImporter.presentImportResult(FileImportHandler.failedGamePickerMessage(errorDescription: error.localizedDescription))
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
                            fileImporter.presentImportResult(FileImportHandler.pnachImportNeedsGameMessage)
                        }
                        pendingPNACHGameName = nil
                    case .failure(let error):
                        if !FileImportHandler.isUserCancelledPickerError(error) {
                            fileImporter.presentImportResult(FileImportHandler.failedPNACHPickerMessage(errorDescription: error.localizedDescription))
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
            .sheet(item: $gameCompatibilityTarget) { game in
                GameCompatibilityPanel(game: game)
                    .presentationDetents([.medium, .large])
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

    private var coverFlowLibrary: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            LazyHStack(alignment: .center, spacing: 20) {
                if let gameName = appState.runningGameName {
                    vmStatusCoverCard(gameName: gameName)
                }

                ForEach(games) { game in
                    coverFlowCard(game)
                }
            }
            .padding(.horizontal, 32)
            .padding(.vertical, 18)
        }
        .background(
            LinearGradient(
                colors: [Color(.systemGroupedBackground), Color(.secondarySystemGroupedBackground)],
                startPoint: .top,
                endPoint: .bottom
            )
        )
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

    private func coverFlowCard(_ game: ISOEntry) -> some View {
        Button {
            open(game)
        } label: {
            VStack(spacing: 12) {
                ZStack(alignment: .topTrailing) {
                    coverThumbnail(for: game, width: 150, height: 225)
                        .shadow(color: .black.opacity(0.28), radius: 18, y: 10)

                    Button {
                        toggleFavorite(game.name)
                    } label: {
                        Image(systemName: game.isFavorite ? "star.fill" : "star")
                            .font(.headline.weight(.semibold))
                            .foregroundStyle(game.isFavorite ? .yellow : .white.opacity(0.88))
                            .padding(8)
                            .background(.black.opacity(0.48), in: Circle())
                    }
                    .buttonStyle(.plain)
                    .padding(8)
                }

                VStack(spacing: 4) {
                    Text(coverStore.displayName(forGameName: game.name))
                        .font(.headline.weight(.semibold))
                        .multilineTextAlignment(.center)
                        .lineLimit(2)
                    Text("\(game.name.pathExtensionLabel)  \(formatSize(game.size))")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                .frame(width: 164)
            }
            .padding(12)
            .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 24, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: 24, style: .continuous)
                    .strokeBorder(game.name == appState.runningGameName ? .green.opacity(0.7) : .white.opacity(0.12), lineWidth: 1)
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

    private func vmStatusCoverCard(gameName: String) -> some View {
        VStack(spacing: 14) {
            Image(systemName: "play.circle.fill")
                .font(.system(size: 48))
                .foregroundStyle(.green)
            Text(settings.localized("Now Running"))
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
            Text(gameName == "BIOS" ? settings.localized("BIOS Only") : gameName)
                .font(.headline.weight(.semibold))
                .lineLimit(2)
                .multilineTextAlignment(.center)
            HStack(spacing: 8) {
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
        }
        .frame(width: 166, height: 276)
        .padding(12)
        .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 24, style: .continuous))
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
            gameCompatibilityTarget = game
        } label: {
            Label(settings.localized("Compatibility Lab"), systemImage: "wand.and.stars")
        }

        Button {
            pendingPNACHGameName = game.name
            showPNACHImporter = true
        } label: {
            Label(settings.localized("Import PNACH / 60 FPS Patch"), systemImage: "wand.and.stars")
        }

        Menu {
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
        } label: {
            Label(settings.localized("Covers"), systemImage: "photo.stack")
        }

        Divider()

        Menu {
            Button {
                clearGameCache(game)
            } label: {
                Label(settings.localized("Clear Game Cache"), systemImage: "trash.slash")
            }

            Button(role: .destructive) {
                pendingDeleteDataGame = game
            } label: {
                Label(settings.localized("Delete Game Data"), systemImage: "externaldrive.badge.xmark")
            }

            Button(role: .destructive) {
                pendingDeleteGame = game
            } label: {
                Label(settings.localized("Delete Game"), systemImage: "trash")
            }
        } label: {
            Label(settings.localized("Game Data"), systemImage: "externaldrive")
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

    private func clearGameCache(_ game: ISOEntry) {
        gameActionTitle = "Clear Game Cache"
        gameActionMessage = ARMSX2Bridge.clearCache(forISO: game.name)
    }

    private func deleteGameData(_ game: ISOEntry) {
        gameActionTitle = "Delete Game Data"
        gameActionMessage = ARMSX2Bridge.deleteGameData(forISO: game.name)
    }

    private func deleteGame(_ game: ISOEntry, deleteData: Bool) {
        if appState.runningGameName == game.name {
            gameActionTitle = "Delete Game"
            gameActionMessage = settings.localized("Stop this game before deleting it.")
            return
        }

        let success = ARMSX2Bridge.deleteISO(game.name, deleteGameData: deleteData)
        if success {
            coverStore.removeManagedCovers(forGameNamed: game.name)
            loadGames()
        }
        gameActionTitle = "Delete Game"
        gameActionMessage = success ? settings.localized("Game deleted.") : settings.localized("Could not delete this game file.")
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
    @State private var settings = SettingsStore.shared

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

                Section(settings.localized("Disc")) {
                    LabeledContent(settings.localized("Region")) {
                        Text(regionDisplay)
                    }
                    LabeledContent(settings.localized("Serial")) {
                        Text(metadataValue("serial"))
                            .textSelection(.enabled)
                    }
                    LabeledContent(settings.localized("CRC")) {
                        Text(metadataValue("crc"))
                            .textSelection(.enabled)
                    }
                    LabeledContent(settings.localized("Format")) {
                        Text(game.name.pathExtensionLabel)
                    }
                    LabeledContent(settings.localized("Size")) {
                        Text(formatSize(game.size))
                    }
                }

                Section(settings.localized("File")) {
                    Text(game.fileURL?.path ?? settings.localized("File path unavailable"))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .textSelection(.enabled)
                }
            }
            .navigationTitle(settings.localized("Game Info"))
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Done")) {
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
        return value.isEmpty ? settings.localized("Unknown") : value
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

private struct GameCompatibilityPanel: View {
    @Environment(\.dismiss) private var dismiss
    @State private var settings = SettingsStore.shared
    @State private var selectedPreset: String
    @State private var identity: String
    @State private var statusMessage: String?

    let game: ISOEntry

    init(game: ISOEntry) {
        self.game = game
        _selectedPreset = State(initialValue: ARMSX2Bridge.compatibilityPreset(forISO: game.name))
        _identity = State(initialValue: ARMSX2Bridge.compatibilityIdentity(forISO: game.name))
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    LabeledContent(settings.localized("Current Game")) {
                        Text(identity.isEmpty ? settings.localized("Unknown") : identity)
                            .foregroundStyle(.secondary)
                    }
                    LabeledContent(settings.localized("Current Mode")) {
                        Text(settings.localized(presetTitle(selectedPreset)))
                            .foregroundStyle(.secondary)
                    }
                } header: {
                    Text(settings.localized("Status"))
                } footer: {
                    Text(settings.localized("Presets are saved for this game and apply on the next boot/reset. Use Off / Default when a preset makes rendering or stability worse."))
                }

                Section(settings.localized("Presets")) {
                    ForEach(compatibilityPresets) { preset in
                        Button {
                            apply(preset)
                        } label: {
                            HStack {
                                Label(settings.localized(preset.title), systemImage: preset.systemImage)
                                Spacer()
                                if selectedPreset == preset.id {
                                    Image(systemName: "checkmark")
                                }
                            }
                        }
                        .foregroundStyle(.primary)
                    }
                }

                Section {
                    ForEach(advancedPresets) { preset in
                        Toggle(isOn: Binding(
                            get: { ARMSX2Bridge.compatibilityFlag(preset.id, forISO: game.name) },
                            set: { enabled in
                                ARMSX2Bridge.setCompatibilityFlag(preset.id, enabled: enabled, forISO: game.name)
                                selectedPreset = ARMSX2Bridge.compatibilityPreset(forISO: game.name)
                                identity = ARMSX2Bridge.compatibilityIdentity(forISO: game.name)
                                statusMessage = "\(settings.localized("Custom compatibility flags saved for")) \(identity)"
                            }
                        )) {
                            Label(settings.localized(preset.title), systemImage: preset.systemImage)
                        }
                    }
                } header: {
                    Text(settings.localized("Advanced Custom Flags"))
                } footer: {
                    Text(settings.localized("Toggle one or more flags when one preset is not enough. Changing any flag switches this game to Custom Advanced Flags."))
                }

                Section {
                    Button(role: .destructive) {
                        ARMSX2Bridge.forgetCompatibilityPreset(forISO: game.name)
                        selectedPreset = ARMSX2Bridge.compatibilityPreset(forISO: game.name)
                        identity = ARMSX2Bridge.compatibilityIdentity(forISO: game.name)
                        statusMessage = settings.localized("Compatibility preset reset for this game.")
                    } label: {
                        Label(settings.localized("Forget This Game's Override"), systemImage: "trash")
                    }
                }

                if let statusMessage {
                    Section {
                        Text(statusMessage)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .navigationTitle(settings.localized("Compatibility Lab"))
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Done")) {
                        dismiss()
                    }
                }
            }
        }
    }

    private func apply(_ preset: CompatibilityPreset) {
        ARMSX2Bridge.setCompatibilityPreset(preset.id, forISO: game.name)
        selectedPreset = ARMSX2Bridge.compatibilityPreset(forISO: game.name)
        identity = ARMSX2Bridge.compatibilityIdentity(forISO: game.name)
        statusMessage = "\(settings.localized(preset.title)) \(settings.localized("saved for this game. Reset or relaunch to apply."))"
    }

    private func presetTitle(_ id: String) -> String {
        compatibilityPresets.first(where: { $0.id == id })?.title ?? "Custom Advanced Flags"
    }

    private var advancedPresets: [CompatibilityPreset] {
        compatibilityPresets.filter { $0.id != "off" }
    }
}

struct PerGameSettingsPanel: View {
    @Environment(\.dismiss) private var dismiss
    @State private var settings = SettingsStore.shared

    let game: ISOEntry
    let onDone: (() -> Void)?

    @State private var enabled: Bool
    @State private var upscaleMultiplier: Float
    @State private var aspectRatio: String
    @State private var textureFiltering: Int
    @State private var hardwareMipmapping: Bool
    @State private var blendingAccuracy: Int
    @State private var eeCoreType: Int
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
        _eeCoreType = State(initialValue: Self.intValue(info["eeCoreType"], defaultValue: 2))
        _enableCheats = State(initialValue: Self.boolValue(info["enableCheats"], defaultValue: false))
        _enablePatches = State(initialValue: Self.boolValue(info["enablePatches"], defaultValue: true))
        _enableGameFixes = State(initialValue: Self.boolValue(info["enableGameFixes"], defaultValue: true))
        _enableGameDBHardwareFixes = State(initialValue: Self.boolValue(info["enableGameDBHardwareFixes"], defaultValue: true))
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Toggle(settings.localized("Use Per-Game Overrides"), isOn: $enabled)
                    Text(settings.localized("Overrides are saved for this game only and apply on the next boot/reset of this title."))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Section(settings.localized("CPU")) {
                    Picker(settings.localized("EE Core"), selection: $eeCoreType) {
                        Text(settings.localized("ARM64 JIT")).tag(2)
                        Text(settings.localized("Interpreter")).tag(1)
                    }
                    .disabled(!enabled)

                    Text(settings.localized("Interpreter is slower, but can help isolate EE JIT crashes for specific games. Reset/relaunch after changing it."))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                Section(settings.localized("Graphics")) {
                    Picker(settings.localized("Internal Resolution"), selection: $upscaleMultiplier) {
                        Text("0.25x (Fastest)").tag(Float(0.25))
                        Text("0.5x").tag(Float(0.5))
                        Text("0.75x").tag(Float(0.75))
                        Text("1x Native").tag(Float(1.0))
                        Text("2x").tag(Float(2.0))
                        Text("3x").tag(Float(3.0))
                        Text("4x").tag(Float(4.0))
                    }
                    .disabled(!enabled)

                    Picker(settings.localized("Aspect Ratio"), selection: $aspectRatio) {
                        Text("Auto 4:3 / 3:2").tag("Auto 4:3/3:2")
                        Text("4:3").tag("4:3")
                        Text("16:9").tag("16:9")
                        Text("10:7").tag("10:7")
                        Text("Stretch").tag("Stretch")
                    }
                    .disabled(!enabled)

                    Picker(settings.localized("Texture Filtering"), selection: $textureFiltering) {
                        Text("Nearest").tag(0)
                        Text("Bilinear Forced").tag(1)
                        Text("Bilinear PS2 Default").tag(2)
                        Text("Bilinear excl. Sprite").tag(3)
                    }
                    .disabled(!enabled)

                    Toggle(settings.localized("Hardware Mipmapping"), isOn: $hardwareMipmapping)
                        .disabled(!enabled)
                    Text(settings.localized("Turn this off only for games with mipmap-related texture stripes, shimmer, or bad LOD. Reset/relaunch the game after changing it."))
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    Picker(settings.localized("Blending Accuracy"), selection: $blendingAccuracy) {
                        Text("Minimum").tag(0)
                        Text("Basic").tag(1)
                        Text("Medium").tag(2)
                        Text("High").tag(3)
                        Text("Full").tag(4)
                        Text("Ultra").tag(5)
                    }
                    .disabled(!enabled)
                }

                Section(settings.localized("Patches & Cheats")) {
                    Toggle(settings.localized("Enable PNACH Cheats"), isOn: $enableCheats)
                        .disabled(!enabled)
                    Toggle(settings.localized("GameDB PNACH Patches"), isOn: $enablePatches)
                        .disabled(!enabled)
                    Toggle(settings.localized("GameDB Core Fixes"), isOn: $enableGameFixes)
                        .disabled(!enabled)
                    Toggle(settings.localized("GameDB Graphics Fixes"), isOn: $enableGameDBHardwareFixes)
                        .disabled(!enabled)
                    Text(settings.localized("If a game looks worse after GameDB, turn off GameDB Graphics Fixes for this game and reset/relaunch it. Core fixes cover timing, clamps, and other compatibility behavior."))
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
            .navigationTitle(settings.localized("Per-Game Settings"))
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(settings.localized("Done")) {
                        if let onDone {
                            onDone()
                        } else {
                            dismiss()
                        }
                    }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Save")) {
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
            eeCoreType: Int32(eeCoreType),
            enableCheats: enableCheats,
            enablePatches: enablePatches,
            enableGameFixes: enableGameFixes,
            enableGameDBHardwareFixes: enableGameDBHardwareFixes
        )
        statusMessage = enabled ? "\(settings.localized("Saved for")) \(game.metadata["serial"] ?? game.name). \(settings.localized("Reset or relaunch the game to apply."))" : settings.localized("Per-game overrides cleared.")
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

private extension String {
    var pathExtensionLabel: String {
        let ext = (self as NSString).pathExtension.uppercased()
        return ext.isEmpty ? "FILE" : ext
    }
}
