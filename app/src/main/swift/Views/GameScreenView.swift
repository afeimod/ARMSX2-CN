// GameScreenView.swift — Unified game screen (Metal + Virtual Pad + Menu)
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

private let runtimeMenuStateChangedNotification = Notification.Name("ARMSX2iOSRuntimeMenuStateChanged")

struct CompatibilityPreset: Identifiable {
    let id: String
    let title: String
    let systemImage: String
}

let compatibilityPresets: [CompatibilityPreset] = [
    CompatibilityPreset(id: "off", title: "Off / Default", systemImage: "power"),
    CompatibilityPreset(id: "cop1", title: "COP1 Everything Only", systemImage: "function"),
    CompatibilityPreset(id: "loadstore", title: "COP1 + EE Load/Store", systemImage: "arrow.left.arrow.right"),
    CompatibilityPreset(id: "mmi", title: "COP1 + EE MMI", systemImage: "rectangle.3.group"),
    CompatibilityPreset(id: "cop2vu", title: "COP1 + EE COP2/VU Macro", systemImage: "cube.transparent"),
    CompatibilityPreset(id: "multdiv", title: "COP1 + EE Mult/Div", systemImage: "multiply"),
    CompatibilityPreset(id: "shifts", title: "COP1 + EE Shifts", systemImage: "arrow.left.and.right"),
    CompatibilityPreset(id: "moves", title: "COP1 + EE Moves/HI-LO", systemImage: "arrow.triangle.swap"),
    CompatibilityPreset(id: "integeralu", title: "COP1 + EE Integer ALU", systemImage: "plus.forwardslash.minus"),
    CompatibilityPreset(id: "branches", title: "COP1 + EE Branches/Jumps", systemImage: "arrow.triangle.branch"),
]

struct GameScreenView: View {
    @State private var appState = AppState.shared
    @State private var settings = SettingsStore.shared
    @State private var fileImporter = FileImportHandler.shared
    @State private var padVisible = true
    @State private var fullScreen = false
    @State private var menuButtonHidden = false
    @State private var vmMenuAvailable = false
    @State private var gameMenuAvailable = false
    @State private var showSaveStates = false
    @State private var showSpeedControl = false
    @State private var showCompatibilityLab = false
    @State private var showPerGameSettings = false
    @State private var showPNACHImporter = false
    @State private var showPadLayoutEditor = false
    @State private var showResetConfirmation = false
    @State private var runtimePerGameSettingsEntry: ISOEntry?
    @State private var compatibilityPresetKey = "off"
    @State private var compatibilityIdentity = ""
    @State private var compatibilityAutoPresets = true
    @State private var saveStateStatus: String? = nil
    @State private var saveStateStatusGeneration = 0
    @State private var runtimeOverlayPauseActive = false
    @State private var previousHideHomeIndicator = false

    private static let briefStatusDisplayDuration: TimeInterval = 2.2
    private static let importantStatusDisplayDuration: TimeInterval = 6.0

    var body: some View {
        GeometryReader { geo in
            let isLandscape = geo.size.width > geo.size.height

            if isLandscape {
                // Landscape: always use full screen area for Metal + pad
                // so that pad coordinates match the layout editor exactly.
                ZStack {
                    MetalGameView()
                    if padVisible {
                        VirtualControllerView(isLandscape: true)
                    }
                    menuOverlay(isLandscape: true)
                }
                .ignoresSafeArea()  // Always fullscreen layout in landscape
            } else {
                // Portrait: Metal top half, pad bottom half. Full-phone Manic
                // skins are intentionally not drawn here until their info.json
                // viewport metadata is parsed, otherwise they crop/stretch over
                // gameplay and make the touch zones look wrong.
                ZStack {
                    VStack(spacing: 0) {
                        MetalGameView()
                            .frame(height: geo.size.height / 2)
                        if padVisible {
                            VirtualControllerView()
                                .frame(height: geo.size.height / 2)
                        } else {
                            Spacer()
                        }
                    }
                    .overlay(alignment: .topTrailing) {
                        menuOverlay(isLandscape: false)
                    }
                }
            }
        }
        .onChange(of: fullScreen) { _, newValue in
            ARMSX2Bridge.setFullScreen(newValue)
        }
        .sheet(isPresented: $showSaveStates) {
            SaveStatesPanel { message, isImportant in
                presentSaveStateStatus(
                    message,
                    displayDuration: isImportant ? Self.importantStatusDisplayDuration : Self.briefStatusDisplayDuration
                )
            }
        }
        .sheet(isPresented: $showSpeedControl) {
            SpeedControlPanel(settings: settings)
                .presentationDetents([.medium])
        }
        .sheet(isPresented: $showCompatibilityLab) {
            compatibilityLabPanel
                .presentationDetents([.medium, .large])
        }
        .fullScreenCover(isPresented: $showPadLayoutEditor) {
            PadLayoutEditView()
        }
        .sheet(isPresented: $showPNACHImporter) {
            ImportDocumentPicker(
                allowedContentTypes: FileImportHandler.pnachContentTypes,
                allowsMultipleSelection: true
            ) { result in
                showPNACHImporter = false
                switch result {
                case .success(let urls):
                    let message = fileImporter.importPNACHURLs(urls, asCheat: true, presentsAlert: false)
                    presentPNACHImportResult(message)
                case .failure(let error):
                    if !FileImportHandler.isUserCancelledPickerError(error) {
                        fileImporter.presentImportResult(FileImportHandler.failedPNACHPickerMessage(errorDescription: error.localizedDescription))
                    }
                }
            }
        }
        .overlay(alignment: .bottom) {
            saveStateToast
        }
        .overlay {
            if showPerGameSettings {
                runtimePerGameSettingsOverlay
                    .transition(.opacity)
            }
        }
        .alert(settings.localized("Reset ROM?"), isPresented: $showResetConfirmation) {
            Button(settings.localized("Cancel"), role: .cancel) {}
            Button(settings.localized("Reset ROM"), role: .destructive) {
                resetCurrentROM()
            }
        } message: {
            Text(settings.localized("Restart the current game? Unsaved progress will be lost."))
        }
        .onAppear {
            enterGameplaySystemChromeMode()
            applyGameScreenPreferences()
            refreshRuntimeMenuState()
        }
        .onDisappear {
            leaveGameplaySystemChromeMode()
        }
        .simultaneousGesture(
            TapGesture(count: 2).onEnded {
                restoreMenuButtonIfHidden()
            }
        )
        .onChange(of: showSaveStates) { _, _ in updateRuntimeOverlayPause() }
        .onChange(of: showSpeedControl) { _, _ in updateRuntimeOverlayPause() }
        .onChange(of: showCompatibilityLab) { _, _ in updateRuntimeOverlayPause() }
        .onChange(of: showPerGameSettings) { _, _ in updateRuntimeOverlayPause() }
        .onChange(of: showPNACHImporter) { _, _ in updateRuntimeOverlayPause() }
        .onChange(of: showPadLayoutEditor) { _, _ in updateRuntimeOverlayPause() }
        .onReceive(NotificationCenter.default.publisher(for: runtimeMenuStateChangedNotification)) { _ in
            refreshRuntimeMenuState()
        }
        .onReceive(Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()) { _ in
            refreshRuntimeMenuState()
        }
        .persistentSystemOverlays(.hidden)
    }

    private func enterGameplaySystemChromeMode() {
        previousHideHomeIndicator = appState.hideHomeIndicator
        appState.hideHomeIndicator = true
    }

    private func leaveGameplaySystemChromeMode() {
        appState.hideHomeIndicator = previousHideHomeIndicator
    }

    @ViewBuilder
    private func menuOverlay(isLandscape: Bool) -> some View {
        if !menuButtonHidden {
            VStack {
                HStack {
                    Spacer()
                    menuButton(isLandscape: isLandscape)
                }
                .padding(.top, isLandscape ? 8 : 4)
                .padding(.trailing, isLandscape ? 8 : 4)
                Spacer()
            }
        }
    }

    private func menuButton(isLandscape: Bool) -> some View {
        Menu {
            Toggle(isOn: Binding(
                get: { settings.osdPreset != .off },
                set: { newValue in
                    if newValue {
                        settings.osdPreset = .simple
                        ARMSX2Bridge.setPerformanceOverlayVisible(true)
                    } else {
                        settings.osdPreset = .off
                        ARMSX2Bridge.setPerformanceOverlayVisible(false)
                    }
                }
            )) {
                Label(settings.localized("OSD"), systemImage: "speedometer")
            }
            Toggle(isOn: $padVisible) {
                Label(settings.localized("Virtual Pad"), systemImage: "gamecontroller")
            }
            Toggle(isOn: $fullScreen) {
                Label(settings.localized("Full Screen"), systemImage: "arrow.up.left.and.arrow.down.right")
            }
            Toggle(isOn: Binding(
                get: { menuButtonHidden || settings.hideMenuButton },
                set: { newValue in
                    settings.hideMenuButton = newValue
                    menuButtonHidden = newValue
                    if newValue {
                        presentSaveStateStatus(settings.localized("Double-tap empty gameplay space to show the menu button again."))
                    }
                }
            )) {
                Label(settings.localized("Hide Menu Button"), systemImage: "eye.slash")
            }
            Button {
                showPadLayoutEditor = true
            } label: {
                Label(settings.localized("Edit Virtual Pad Layout"), systemImage: "square.resize")
            }

            Divider()

            Button {
                refreshCompatibilityState()
                showCompatibilityLab = true
            } label: {
                Label(settings.localized("Compatibility Lab"), systemImage: "wand.and.stars")
            }

            if gameMenuAvailable {
                Button {
                    openPerGameSettingsForCurrentGame()
                } label: {
                    Label(settings.localized("Per-Game Settings"), systemImage: "slider.horizontal.3")
                }
            }

            if vmMenuAvailable {
                Button {
                    showSpeedControl = true
                } label: {
                    Label(settings.localized("Speed / FPS Target"), systemImage: "speedometer")
                }

                Button {
                    showResetConfirmation = true
                } label: {
                    Label(settings.localized("Reset ROM"), systemImage: "arrow.counterclockwise.circle")
                }
            }

            if gameMenuAvailable || vmMenuAvailable {
                Button {
                    showSaveStates = true
                } label: {
                    Label(settings.localized("Save / Load States"), systemImage: "square.stack.3d.up.fill")
                }
            }

            if vmMenuAvailable {
                Menu {
                    Button {
                        ejectDisc()
                    } label: {
                        Label(settings.localized("Eject Disc"), systemImage: "eject")
                    }

                    let discs = availableDiscSwapNames
                    if discs.isEmpty {
                        Text(settings.localized("No disc images found"))
                    } else {
                        Menu {
                            ForEach(discs, id: \.self) { discName in
                                Button {
                                    changeDisc(to: discName)
                                } label: {
                                    Label(discName, systemImage: "opticaldisc")
                                }
                            }
                        } label: {
                            Label(settings.localized("Insert Disc (No Reboot)"), systemImage: "tray.and.arrow.down")
                        }

                        Menu {
                            ForEach(discs, id: \.self) { discName in
                                Button {
                                    restartWithDisc(discName)
                                } label: {
                                    Label(discName, systemImage: "arrow.clockwise.circle")
                                }
                            }
                        } label: {
                            Label(settings.localized("Restart With Disc"), systemImage: "arrow.clockwise.circle")
                        }
                    }
                } label: {
                    Label(settings.localized("Change Disc"), systemImage: "opticaldisc")
                }
            }

            if gameMenuAvailable {
                Button {
                    showPNACHImporter = true
                } label: {
                    Label(settings.localized("Import PNACH / 60 FPS Patch"), systemImage: "wand.and.stars")
                }

                Button {
                    clearCurrentGameCache()
                } label: {
                    Label(settings.localized("Clear Current Game Cache"), systemImage: "trash.slash")
                }
            }

            Divider()
            Button {
                appState.returnToMenu()
            } label: {
                Label(settings.localized("Back to Menu"), systemImage: "list.bullet")
            }
        } label: {
            Image(systemName: "ellipsis.circle.fill")
                .font(.title3)
                .foregroundStyle(.white.opacity(0.5))
                .padding(6)
                .background(.black.opacity(0.15), in: Circle())
        }
    }

    private func applyGameScreenPreferences() {
        menuButtonHidden = settings.hideMenuButton
        if settings.autoFullscreen {
            fullScreen = true
            ARMSX2Bridge.setFullScreen(true)
        }
    }

    private func restoreMenuButtonIfHidden() {
        guard menuButtonHidden else { return }

        menuButtonHidden = false
        settings.hideMenuButton = false
        presentSaveStateStatus(settings.localized("Menu button shown"))
    }

    @ViewBuilder
    private var runtimePerGameSettingsContent: some View {
        if let runtimePerGameSettingsEntry {
            PerGameSettingsPanel(game: runtimePerGameSettingsEntry) {
                closePerGameSettingsOverlay()
            }
        } else {
            NavigationStack {
                ContentUnavailableView(
                    settings.localized("No Game Active"),
                    systemImage: "gamecontroller",
                    description: Text(settings.localized("Start a game before changing per-game settings."))
                )
                .navigationTitle(settings.localized("Per-Game Settings"))
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) {
                        Button(settings.localized("Done")) {
                            closePerGameSettingsOverlay()
                        }
                    }
                }
            }
        }
    }

    private var runtimePerGameSettingsOverlay: some View {
        GeometryReader { geo in
            let isLandscape = geo.size.width > geo.size.height

            ZStack {
                Color.black.opacity(0.42)
                    .ignoresSafeArea()

                runtimePerGameSettingsContent
                    .frame(maxWidth: isLandscape ? 760 : .infinity, maxHeight: isLandscape ? 560 : .infinity)
                    .background(Color(.systemBackground), in: RoundedRectangle(cornerRadius: 34, style: .continuous))
                    .clipShape(RoundedRectangle(cornerRadius: 34, style: .continuous))
                    .shadow(color: .black.opacity(0.35), radius: 24, y: 12)
                    .padding(.horizontal, isLandscape ? 28 : 12)
                    .padding(.vertical, isLandscape ? 18 : 8)
            }
        }
    }

    private func refreshRuntimeMenuState() {
        let vmRunning = ARMSX2Bridge.isVMRunning()
        let gameReady = ARMSX2Bridge.hasValidSaveStateGame()
        if vmMenuAvailable != vmRunning {
            vmMenuAvailable = vmRunning
        }
        if gameMenuAvailable != gameReady {
            gameMenuAvailable = gameReady
        }
        refreshCompatibilityState()
    }

    private func openPerGameSettingsForCurrentGame() {
        guard let entry = makeRuntimePerGameSettingsEntry() else {
            runtimePerGameSettingsEntry = nil
            showPerGameSettings = true
            presentImportantSaveStateStatus(settings.localized("Per-game settings need a running game."))
            return
        }

        runtimePerGameSettingsEntry = entry
        showPerGameSettings = true
    }

    private func closePerGameSettingsOverlay() {
        runtimePerGameSettingsEntry = nil
        showPerGameSettings = false
        refreshRuntimeMenuState()
        ARMSX2Bridge.setFullScreen(fullScreen)
        ARMSX2Bridge.prepareGameRenderViewForCurrentRenderer()

        DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
            ARMSX2Bridge.setFullScreen(fullScreen)
            ARMSX2Bridge.prepareGameRenderViewForCurrentRenderer()
            refreshRuntimeMenuState()
        }
    }

    private func updateRuntimeOverlayPause() {
        let shouldPause = showSaveStates || showSpeedControl || showCompatibilityLab || showPerGameSettings || showPNACHImporter || showPadLayoutEditor
        guard runtimeOverlayPauseActive != shouldPause else { return }

        runtimeOverlayPauseActive = shouldPause
        if ARMSX2Bridge.isVMRunning() {
            ARMSX2Bridge.setVMPaused(shouldPause)
        }
    }

    private func makeRuntimePerGameSettingsEntry() -> ISOEntry? {
        guard let gameName = currentRuntimeGameName() else {
            return nil
        }

        let isoDir = ARMSX2Bridge.isoDirectory()
        let docsDir = ARMSX2Bridge.documentsDirectory()
        let fm = FileManager.default
        var path = (isoDir as NSString).appendingPathComponent(gameName)
        if !fm.fileExists(atPath: path) {
            path = (docsDir as NSString).appendingPathComponent(gameName)
        }

        let fileURL = fm.fileExists(atPath: path) ? URL(fileURLWithPath: path) : nil
        let attrs = try? fm.attributesOfItem(atPath: path)
        let size = attrs?[.size] as? UInt64 ?? 0
        let metadata = ARMSX2Bridge.gameMetadata(forISO: gameName)
        return ISOEntry(
            name: gameName,
            fileURL: fileURL,
            coverURL: nil,
            coverSignature: nil,
            metadata: metadata,
            size: size,
            isFavorite: ARMSX2Bridge.isFavorite(gameName)
        )
    }

    private func currentRuntimeGameName() -> String? {
        if let gameName = normalizedRuntimeGameName(appState.runningGameName) {
            return gameName
        }

        if let gameName = normalizedRuntimeGameName(ARMSX2Bridge.currentGameISOName()) {
            return gameName
        }

        if let gameName = normalizedRuntimeGameName(ARMSX2Bridge.currentISOPath()) {
            return gameName
        }

        let bootISO = ARMSX2Bridge.getINIString("GameISO", key: "BootISO", defaultValue: "")
        if let gameName = normalizedRuntimeGameName(bootISO) {
            return gameName
        }

        return gameNameMatchingRuntimeIdentity()
    }

    private func normalizedRuntimeGameName(_ value: String?) -> String? {
        guard var value = value?.trimmingCharacters(in: .whitespacesAndNewlines),
              !value.isEmpty else {
            return nil
        }

        value = value.trimmingCharacters(in: CharacterSet(charactersIn: "\"'"))
        let fileName = (value as NSString).lastPathComponent
        guard !fileName.isEmpty,
              fileName != "BIOS",
              fileName != "AutoBoot" else {
            return nil
        }

        return fileName
    }

    private func gameNameMatchingRuntimeIdentity() -> String? {
        let identity = normalizedRuntimeIdentity(ARMSX2Bridge.compatibilityIdentityForCurrentGame())
        guard !identity.isEmpty else {
            return nil
        }

        for gameName in ARMSX2Bridge.availableISOs() {
            let metadata = ARMSX2Bridge.gameMetadata(forISO: gameName)
            let serial = normalizedRuntimeIdentity(metadata["serial"])
            if !serial.isEmpty && serial == identity {
                return gameName
            }

            if let crc = metadata["crc"]?.trimmingCharacters(in: .whitespacesAndNewlines).uppercased(),
               !crc.isEmpty,
               (identity == crc || identity == "CRC-\(crc)") {
                return gameName
            }
        }

        return nil
    }

    private func normalizedRuntimeIdentity(_ value: String?) -> String {
        (value ?? "")
            .replacingOccurrences(of: "_", with: "-")
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .uppercased()
    }

    private var compatibilityLabPanel: some View {
        NavigationStack {
            Form {
                let currentPreset = compatibilityPreset(for: compatibilityPresetKey)

                Section {
                    Toggle(isOn: Binding(
                        get: { compatibilityAutoPresets },
                        set: { newValue in
                            compatibilityAutoPresets = newValue
                            ARMSX2Bridge.setCompatibilityAutoGamePresetsEnabled(newValue)
                            refreshCompatibilityState()
                        }
                    )) {
                        Label(settings.localized("Auto Game Presets"), systemImage: "sparkles")
                    }

                    LabeledContent(settings.localized("Current Mode")) {
                        Text(settings.localized(currentPreset.title))
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.trailing)
                    }

                    if !compatibilityIdentity.isEmpty {
                        LabeledContent(settings.localized("Current Game")) {
                            Text(compatibilityIdentity)
                                .foregroundStyle(.secondary)
                        }
                    } else {
                        Text(settings.localized("Start a game to remember presets per title."))
                            .foregroundStyle(.secondary)
                    }
                } header: {
                    Text(settings.localized("Status"))
                } footer: {
                    Text(settings.localized("Auto Game Presets applies known safe defaults. Manual flags below are remembered for the current game when a game is running."))
                }

                Section {
                    Button {
                        applyCompatibilityPreset(compatibilityPreset(for: "off"))
                    } label: {
                        Label(settings.localized("Use Default / Clear Flags"), systemImage: "power")
                    }
                    .foregroundStyle(.primary)
                } header: {
                    Text(settings.localized("Reset"))
                } footer: {
                    Text(settings.localized("Use this when testing is done or a game behaves worse with compatibility flags enabled."))
                }

                Section {
                    compatibilityLabToggle(
                        "COP1EverythingOnly",
                        title: "COP1 Everything Only",
                        systemImage: "function"
                    )
                    compatibilityLabToggle(
                        "COP1EverythingPlusLoadStore",
                        title: "COP1 Everything + EE Load/Store",
                        systemImage: "arrow.left.arrow.right"
                    )
                    compatibilityLabToggle(
                        "COP1EverythingPlusMMI",
                        title: "COP1 Everything + EE MMI",
                        systemImage: "rectangle.3.group"
                    )
                    compatibilityLabToggle(
                        "COP1EverythingPlusCOP2VU",
                        title: "COP1 Everything + EE COP2/VU Macro",
                        systemImage: "cube.transparent"
                    )
                    compatibilityLabToggle(
                        "COP1EverythingPlusMultDiv",
                        title: "COP1 Everything + EE Mult/Div",
                        systemImage: "multiply"
                    )
                    compatibilityLabToggle(
                        "COP1EverythingPlusShifts",
                        title: "COP1 Everything + EE Shifts",
                        systemImage: "arrow.left.and.right"
                    )
                    compatibilityLabToggle(
                        "COP1EverythingPlusMoves",
                        title: "COP1 Everything + EE Moves/HI-LO",
                        systemImage: "arrow.triangle.swap"
                    )
                    compatibilityLabToggle(
                        "COP1EverythingPlusIntegerALU",
                        title: "COP1 Everything + EE Integer ALU",
                        systemImage: "plus.forwardslash.minus"
                    )
                    compatibilityLabToggle(
                        "COP1EverythingPlusBranches",
                        title: "COP1 Everything + EE Branches/Jumps",
                        systemImage: "arrow.triangle.branch"
                    )
                } header: {
                    Text(settings.localized("Manual Compatibility Flags"))
                } footer: {
                    Text(settings.localized("Toggle one or more flags when a game needs compatibility help. Changing any flag switches this game to Custom Advanced Flags."))
                }

                if !compatibilityIdentity.isEmpty {
                    Section {
                        Button(role: .destructive) {
                            let identity = compatibilityIdentity
                            ARMSX2Bridge.forgetCompatibilityPresetForCurrentGame()
                            refreshCompatibilityState()
                            presentSaveStateStatus("\(settings.localized("Compatibility preset reset for")) \(identity)")
                        } label: {
                            Label(settings.localized("Forget This Game's Override"), systemImage: "trash")
                        }
                    }
                }
            }
            .navigationTitle(settings.localized("Compatibility Lab"))
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Done")) {
                        showCompatibilityLab = false
                    }
                }
            }
            .onAppear(perform: refreshCompatibilityState)
        }
    }

    private func compatibilityLabToggle(_ key: String, title: String, systemImage: String) -> some View {
        Toggle(isOn: Binding(
            get: { ARMSX2Bridge.getJITBisectFlag(key, defaultValue: false) },
            set: {
                ARMSX2Bridge.setJITBisectFlag(key, value: $0)
                compatibilityPresetKey = "custom"
                if !compatibilityIdentity.isEmpty {
                    presentSaveStateStatus("\(settings.localized("Custom compatibility flags saved for")) \(compatibilityIdentity)")
                }
            }
        )) {
            Label(settings.localized(title), systemImage: systemImage)
        }
    }

    private func refreshCompatibilityState() {
        let preset = ARMSX2Bridge.compatibilityPresetForCurrentGame()
        let identity = ARMSX2Bridge.compatibilityIdentityForCurrentGame()
        let autoPresets = ARMSX2Bridge.isCompatibilityAutoGamePresetsEnabled()

        if compatibilityPresetKey != preset {
            compatibilityPresetKey = preset
        }
        if compatibilityIdentity != identity {
            compatibilityIdentity = identity
        }
        if compatibilityAutoPresets != autoPresets {
            compatibilityAutoPresets = autoPresets
        }
    }

    private func compatibilityPreset(for id: String) -> CompatibilityPreset {
        if let preset = compatibilityPresets.first(where: { $0.id == id }) {
            return preset
        }

        return CompatibilityPreset(id: "custom", title: "Custom Advanced Flags", systemImage: "slider.horizontal.3")
    }

    private func applyCompatibilityPreset(_ preset: CompatibilityPreset) {
        let rememberForCurrentGame = !compatibilityIdentity.isEmpty
        ARMSX2Bridge.setCompatibilityPreset(preset.id, rememberForCurrentGame: rememberForCurrentGame)
        refreshCompatibilityState()

        if rememberForCurrentGame {
            presentSaveStateStatus("\(settings.localized(preset.title)) \(settings.localized("saved for")) \(compatibilityIdentity)")
        } else {
            presentSaveStateStatus("\(settings.localized("Compatibility preset set to")) \(settings.localized(preset.title))")
        }
    }

    private func resetCurrentROM() {
        appState.resetCurrentVM()
        presentSaveStateStatus(settings.localized("Restarting ROM..."))
    }

    private func clearCurrentGameCache() {
        guard let gameName = currentRuntimeGameName() else {
            presentImportantSaveStateStatus(settings.localized("Cache clear needs a running game."))
            return
        }

        let message = ARMSX2Bridge.clearCache(forISO: gameName)
        presentSaveStateStatus(message)
    }

    @ViewBuilder
    private var saveStateToast: some View {
        if let saveStateStatus {
            Text(saveStateStatus)
                .font(.callout.weight(.semibold))
                .foregroundStyle(.white)
                .padding(.horizontal, 14)
                .padding(.vertical, 10)
                .background(.black.opacity(0.72), in: Capsule())
                .padding(.bottom, 24)
                .transition(.opacity.combined(with: .move(edge: .bottom)))
        }
    }

    private func presentSaveStateStatus(
        _ message: String,
        displayDuration: TimeInterval = Self.briefStatusDisplayDuration
    ) {
        saveStateStatusGeneration += 1
        let currentGeneration = saveStateStatusGeneration
        withAnimation(.easeOut(duration: 0.18)) {
            saveStateStatus = message
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + displayDuration) {
            guard saveStateStatusGeneration == currentGeneration else { return }
            withAnimation(.easeIn(duration: 0.18)) {
                saveStateStatus = nil
            }
        }
    }

    private func presentImportantSaveStateStatus(_ message: String) {
        presentSaveStateStatus(message, displayDuration: Self.importantStatusDisplayDuration)
    }

    private func presentPNACHImportResult(_ message: String) {
        if Self.isPNACHImportSuccessMessage(message) {
            presentSaveStateStatus(message)
        } else {
            fileImporter.presentImportResult(message)
        }
    }

    private static func isPNACHImportSuccessMessage(_ message: String) -> Bool {
        let lines = message
            .split(separator: "\n")
            .map { String($0).trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
        return !lines.isEmpty && lines.allSatisfy { $0.hasPrefix("PNACH imported") }
    }

    private var availableDiscSwapNames: [String] {
        ARMSX2Bridge.availableISOs().filter { !$0.lowercased().hasSuffix(".elf") }
    }

    private func changeDisc(to discName: String) {
        presentSaveStateStatus("Changing disc...")
        ARMSX2Bridge.changeDisc(toISO: discName) { success in
            Task { @MainActor in
                if success {
                    presentSaveStateStatus("\(discName) inserted. Use the game's disc-swap prompt if needed.")
                } else {
                    presentImportantSaveStateStatus("Could not change discs. Open the game's disc-swap prompt first, or restart with the target disc.")
                }
            }
        }
    }

    private func restartWithDisc(_ discName: String) {
        presentSaveStateStatus("Restarting with \(discName)...")
        appState.shutdownAndBoot(isoName: discName)
    }

    private func ejectDisc() {
        presentSaveStateStatus("Ejecting disc...")
        ARMSX2Bridge.ejectDisc { success in
            Task { @MainActor in
                if success {
                    presentSaveStateStatus("Disc ejected")
                } else {
                    presentImportantSaveStateStatus("Could not eject the disc. Try again after the game has finished loading.")
                }
            }
        }
    }
}

private struct SaveStatesPanel: View {
    @Environment(\.dismiss) private var dismiss
    @State private var settings = SettingsStore.shared
    @State private var slots: [ARMSX2SaveStateSlotInfo] = []
    @State private var busySlot: Int? = nil
    @State private var pendingOverwrite: ARMSX2SaveStateSlotInfo? = nil

    let statusHandler: (String, Bool) -> Void

    var body: some View {
        NavigationStack {
            ScrollView {
                if slots.isEmpty {
                    VStack(spacing: 10) {
                        Image(systemName: "hourglass")
                            .font(.largeTitle)
                            .foregroundStyle(.secondary)
                        Text(settings.localized("Save states are not ready yet."))
                            .font(.headline)
                        Text(settings.localized("Wait until the game has fully identified, then try again."))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(32)
                } else {
                    LazyVStack(spacing: 10) {
                        ForEach(slots, id: \.slot) { slot in
                            SaveStateSlotRow(
                                info: slot,
                                isBusy: busySlot == slot.slot,
                                onSave: { save(slot) },
                                onLoad: { load(slot) },
                                onOverwrite: { pendingOverwrite = slot }
                            )
                        }
                    }
                    .padding()
                }
            }
            .safeAreaInset(edge: .top) {
                Text(settings.localized("Empty slots can save. Occupied slots can load or overwrite."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal)
                    .padding(.vertical, 8)
                    .background(.regularMaterial)
            }
            .navigationTitle(settings.localized("Save / Load States"))
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button(settings.localized("Done")) {
                        dismiss()
                    }
                }
            }
            .onAppear(perform: refresh)
            .onReceive(NotificationCenter.default.publisher(for: runtimeMenuStateChangedNotification)) { _ in
                refresh()
            }
            .confirmationDialog(
                "\(settings.localized("Overwrite Slot")) \(pendingOverwrite?.slot ?? 0)?",
                isPresented: Binding(
                    get: { pendingOverwrite != nil },
                    set: { newValue in
                        if !newValue {
                            pendingOverwrite = nil
                        }
                    }
                ),
                titleVisibility: .visible
            ) {
                Button(settings.localized("Overwrite"), role: .destructive) {
                    if let pendingOverwrite {
                        save(pendingOverwrite)
                    }
                    pendingOverwrite = nil
                }
                Button(settings.localized("Cancel"), role: .cancel) {
                    pendingOverwrite = nil
                }
            }
        }
    }

    private func refresh() {
        slots = ARMSX2Bridge.saveStateSlots()
    }

    private func save(_ slot: ARMSX2SaveStateSlotInfo) {
        let slotNumber = slot.slot
        busySlot = slotNumber
        ARMSX2Bridge.saveState(toSlot: slotNumber) { success in
            Task { @MainActor in
                busySlot = nil
                refresh()
                let message = success
                    ? "\(settings.localized("State saved to slot")) \(slotNumber)"
                    : "\(settings.localized("Could not save slot")) \(slotNumber). \(settings.localized("Try again after gameplay has fully loaded."))"
                statusHandler(message, !success)
            }
        }
    }

    private func load(_ slot: ARMSX2SaveStateSlotInfo) {
        let slotNumber = slot.slot
        busySlot = slotNumber
        ARMSX2Bridge.loadState(fromSlot: slotNumber) { success in
            Task { @MainActor in
                busySlot = nil
                refresh()
                let message = success
                    ? "\(settings.localized("State loaded from slot")) \(slotNumber)"
                    : "\(settings.localized("Could not load slot")) \(slotNumber). \(settings.localized("Make sure it has a saved state first."))"
                statusHandler(message, !success)
                if success {
                    dismiss()
                }
            }
        }
    }
}

private struct SpeedControlPanel: View {
    @Bindable var settings: SettingsStore
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            Form {
                Section(settings.localized("Frame Limiter")) {
                    Toggle(settings.localized("Enable Limiter"), isOn: $settings.frameLimiterEnabled)

                    if settings.frameLimiterEnabled {
                        VStack(alignment: .leading, spacing: 10) {
                            HStack {
                                Text(settings.localized("FPS Target"))
                                Spacer()
                                Text(Self.formatFPS(settings.targetFPS))
                                    .foregroundStyle(.secondary)
                                    .font(.callout.monospacedDigit())
                            }

                            Slider(
                                value: $settings.targetFPS,
                                in: SettingsStore.minTargetFPS...SettingsStore.maxTargetFPS,
                                step: 1.0
                            )

                            HStack {
                                quickTargetButton(30)
                                quickTargetButton(45)
                                quickTargetButton(60)
                                quickTargetButton(90)
                                quickTargetButton(120)
                            }
                        }
                    } else {
                        Text(settings.localized("Limiter is OFF. Games can run above normal speed and may draw more power."))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }

                Section(settings.localized("How It Works")) {
                    Text(settings.localized("This controls PCSX2 Normal Speed. On NTSC games, 60 FPS is normal speed and 30 FPS is about 50% speed. It is safe to change while a game is running."))
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    HStack {
                        Text(settings.localized("Normal Speed"))
                        Spacer()
                        Text(Self.formatPercent(settings.targetFPS / max(settings.ntscFramerate, 1.0)))
                            .foregroundStyle(.secondary)
                            .font(.callout.monospacedDigit())
                    }
                }
            }
            .navigationTitle(settings.localized("Speed / FPS Target"))
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Done")) {
                        dismiss()
                    }
                }
            }
        }
    }

    private func quickTargetButton(_ fps: Float) -> some View {
        Button(Self.formatCompactFPS(fps)) {
            settings.frameLimiterEnabled = true
            settings.targetFPS = fps
        }
        .buttonStyle(.bordered)
        .font(.caption.monospacedDigit())
    }

    private static func formatFPS(_ value: Float) -> String {
        String(format: "%.0f FPS", value)
    }

    private static func formatCompactFPS(_ value: Float) -> String {
        String(format: "%.0f", value)
    }

    private static func formatPercent(_ scalar: Float) -> String {
        String(format: "%.0f%%", scalar * 100.0)
    }
}

private struct SaveStateSlotRow: View {
    let info: ARMSX2SaveStateSlotInfo
    let isBusy: Bool
    let onSave: () -> Void
    let onLoad: () -> Void
    let onOverwrite: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 12) {
                SaveStatePreview(data: info.previewPNGData, occupied: info.occupied)
                    .frame(width: 96, height: 72)

                VStack(alignment: .leading, spacing: 4) {
                    Text("Slot \(info.slot)")
                        .font(.headline)

                    if info.occupied {
                        if let modifiedDate = info.modifiedDate {
                            Text(Self.dateFormatter.string(from: modifiedDate))
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Text(info.fileName)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    } else {
                        Text("Empty")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }

                Spacer(minLength: 8)

                if isBusy {
                    ProgressView()
                        .frame(width: 88)
                } else if !info.occupied {
                    Button(action: onSave) {
                        Label("Save", systemImage: "square.and.arrow.down")
                    }
                    .buttonStyle(.borderedProminent)
                }
            }

            if info.occupied && !isBusy {
                HStack(spacing: 8) {
                    Button(action: onLoad) {
                        Label("Load", systemImage: "arrow.down.circle")
                    }
                    .buttonStyle(.borderedProminent)
                    .frame(maxWidth: .infinity)

                    Button(action: onOverwrite) {
                        Label("Overwrite", systemImage: "arrow.triangle.2.circlepath")
                    }
                    .buttonStyle(.bordered)
                    .frame(maxWidth: .infinity)
                }
            }
        }
        .padding(12)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8, style: .continuous))
    }

    private static let dateFormatter: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateStyle = .medium
        formatter.timeStyle = .short
        return formatter
    }()
}

private struct SaveStatePreview: View {
    let data: Data?
    let occupied: Bool

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 6, style: .continuous)
                .fill(.black.opacity(0.12))

            if let data, let image = UIImage(data: data) {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFill()
                    .clipShape(RoundedRectangle(cornerRadius: 6, style: .continuous))
            } else {
                Image(systemName: occupied ? "photo" : "tray")
                    .font(.title2)
                    .foregroundStyle(.secondary)
            }
        }
        .clipped()
    }
}
