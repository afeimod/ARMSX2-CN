// GameScreenView.swift — Unified game screen (Metal + Virtual Pad + Menu)
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit
import GameController

private let runtimeMenuStateChangedNotification = Notification.Name("ARMSX2iOSRuntimeMenuStateChanged")
private let retroAchievementsToastNotification = Notification.Name("ARMSX2RetroAchievementsNotification")

private struct RetroAchievementsToast: Equatable {
    let title: String
    let message: String
    let badgePath: String?
}

private struct RetroAchievementEntry: Identifiable, Equatable {
    let id: Int
    let title: String
    let description: String
    let badgePath: String?
    let measuredProgress: String
    let points: Int
    let unlockTime: Int
    let state: Int
    let category: Int
    let bucket: Int
    let unlocked: Int
    let measuredPercent: Double
    let rarity: Double
    let rarityHardcore: Double

    init?(dictionary: [String: Any]) {
        guard let idNumber = dictionary["id"] as? NSNumber else { return nil }
        id = idNumber.intValue
        title = dictionary["title"] as? String ?? ""
        description = dictionary["description"] as? String ?? ""
        let rawBadgePath = (dictionary["badgePath"] as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        badgePath = rawBadgePath.isEmpty ? nil : rawBadgePath
        measuredProgress = dictionary["measuredProgress"] as? String ?? ""
        points = (dictionary["points"] as? NSNumber)?.intValue ?? 0
        unlockTime = (dictionary["unlockTime"] as? NSNumber)?.intValue ?? 0
        state = (dictionary["state"] as? NSNumber)?.intValue ?? 0
        category = (dictionary["category"] as? NSNumber)?.intValue ?? 0
        bucket = (dictionary["bucket"] as? NSNumber)?.intValue ?? 0
        unlocked = (dictionary["unlocked"] as? NSNumber)?.intValue ?? 0
        measuredPercent = (dictionary["measuredPercent"] as? NSNumber)?.doubleValue ?? 0
        rarity = (dictionary["rarity"] as? NSNumber)?.doubleValue ?? 0
        rarityHardcore = (dictionary["rarityHardcore"] as? NSNumber)?.doubleValue ?? 0
    }

    var isUnlocked: Bool { state == 2 || unlocked != 0 }
    var isUnsupported: Bool { state == 3 || bucket == 3 }
    var isUnofficial: Bool { (category & 2) != 0 || bucket == 4 }
    var isActiveChallenge: Bool { bucket == 6 || bucket == 7 }
}

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

private struct GameScreenSizePreferenceKey: PreferenceKey {
    static let defaultValue: CGSize = .zero
    static func reduce(value: inout CGSize, nextValue: () -> CGSize) {
        value = nextValue()
    }
}

struct GameScreenView: View {
    // MARK: - State & Constants

    @State private var appState = AppState.shared
    @State private var settings = SettingsStore.shared
    @State private var layoutPresets = PadLayoutPresetStore.shared
    @State private var skinLibrary = VPadSkinLibraryStore.shared
    @State private var fileImporter = FileImportHandler.shared
    @State private var userVirtualPadVisible = true
    @State private var externalControllerConnected = false
    @State private var fullScreen = false
    @State private var menuButtonHidden = false
    @State private var vmMenuAvailable = false
    @State private var gameMenuAvailable = false
    @State private var showSaveStates = false
    @State private var showSpeedControl = false
    @State private var showCompatibilityLab = false
    @State private var showPerGameSettings = false
    @State private var showPNACHImporter = false
    @State private var showRetroAchievements = false
    @State private var showPadLayoutEditor = false
    @State private var showResetConfirmation = false
    @State private var runtimePerGameSettingsEntry: ISOEntry?
    @State private var runtimePerGameSettings: [String: Any]?
    @State private var runtimePadLayoutIdentity: PadLayoutGameIdentity?
    @State private var compatibilityPresetKey = "off"
    @State private var compatibilityIdentity = ""
    @State private var compatibilityAutoPresets = true
    @State private var statusMessage: String? = nil
    @State private var statusMessageGeneration = 0
    @State private var statusMessageDismissTask: Task<Void, Never>?
    @State private var retroAchievementsToast: RetroAchievementsToast? = nil
    @State private var retroAchievementsToastGeneration = 0
    @State private var retroAchievementsToastDismissTask: Task<Void, Never>?
    @State private var runtimeOverlayPauseActive = false
    @State private var previousHideHomeIndicator = false

    @Environment(\.scenePhase) private var scenePhase

    private static let briefStatusDisplayDuration: TimeInterval = 2.2
    private static let importantStatusDisplayDuration: TimeInterval = 6.0
    private static let retroAchievementsToastDisplayDuration: TimeInterval = 5.0

    private var displaySafeAreaInsets: UIEdgeInsets {
        UIApplication.shared.connectedScenes
            .compactMap { $0 as? UIWindowScene }
            .first?.windows
            .first?.safeAreaInsets ?? .zero
    }

    // MARK: - Body

    var body: some View {
        GeometryReader { geo in
            let isLandscape = geo.size.width > geo.size.height

            Group {
                if isLandscape {
                    // Landscape: full-screen layout so pad coordinates match the layout editor.
                    ZStack {
                        MetalGameView()
                        if effectiveVirtualPadVisible {
                            VirtualControllerView(
                                isLandscape: true,
                                layoutSnapshot: effectivePadLayoutSnapshot,
                                skinDescriptor: effectivePadSkinDescriptor
                            )
                        }
                        menuButtonOverlay(isLandscape: true)
                    }
                    .ignoresSafeArea()
                } else {
                    // Portrait: top game viewport, bottom controller deck.
                    // Game respects the top safe area so OSD stays below the Dynamic Island.
                    // Controller ignores the bottom safe area so buttons remain usable near the home indicator.
                    VStack(spacing: 0) {
                        let gameHeight = min(geo.size.width * 3 / 4, geo.size.height * 0.55)
                        MetalGameView()
                            .frame(height: gameHeight)
                            .clipped()

                        if effectiveVirtualPadVisible {
                            ZStack {
                                Color.black
                                VirtualControllerView(
                                    layoutSnapshot: effectivePadLayoutSnapshot,
                                    skinDescriptor: effectivePadSkinDescriptor
                                )
                                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                            }
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                        }
                    }
                    .overlay(alignment: .topTrailing) {
                        if !menuButtonHidden {
                            menuButton()
                                .padding(.top, 4)
                                .padding(.trailing, 4)
                        }
                    }
                    .ignoresSafeArea(.container, edges: .bottom)
                }
            }
            .preference(key: GameScreenSizePreferenceKey.self, value: geo.size)
        }
        .onPreferenceChange(GameScreenSizePreferenceKey.self) { _ in
            syncFullscreenStateFromWindow()
        }
        .sheet(isPresented: $showSaveStates) {
            SaveStatesPanel { message, isImportant in
                presentStatusMessage(
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
        .sheet(isPresented: $showRetroAchievements) {
            RetroAchievementsGamePanel(settings: settings)
                .presentationDetents([.medium, .large])
        }
        .overlay(alignment: .top) {
            if showPadLayoutEditor {
                PadLayoutEditView(onDismiss: {
                    showPadLayoutEditor = false
                    updateRuntimeOverlayPause()
                }, context: runtimePadLayoutEditorContext)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
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
            statusToastOverlay
        }
        .overlay(alignment: .top) {
            retroAchievementsToastOverlay
        }
        .sheet(isPresented: $showPerGameSettings) {
            runtimePerGameSettingsContent
                .presentationDetents([.large])
                .presentationDragIndicator(.visible)
                .presentationBackground(.regularMaterial)
                .presentationCornerRadius(34)
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
            syncFullscreenStateFromWindow()
            applyInitialFullscreenPreference()
            refreshExternalControllerConnectionState()
            refreshRuntimeMenuState()
        }
        .onDisappear {
            statusMessageDismissTask?.cancel()
            retroAchievementsToastDismissTask?.cancel()
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
        .onChange(of: showRetroAchievements) { _, _ in updateRuntimeOverlayPause() }
        .onChange(of: showPadLayoutEditor) { _, _ in updateRuntimeOverlayPause() }
        .onChange(of: showResetConfirmation) { _, _ in updateRuntimeOverlayPause() }
        .onChange(of: scenePhase) { _, newPhase in
            if newPhase == .active {
                syncFullscreenStateFromWindow()
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: runtimeMenuStateChangedNotification)) { _ in
            refreshRuntimeMenuState()
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in
            refreshExternalControllerConnectionState()
        }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidDisconnect)) { _ in
            refreshExternalControllerConnectionState()
        }
        .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ARMSX2iOSPadLayoutEditorDismissed"))) { _ in
            showPadLayoutEditor = false
            updateRuntimeOverlayPause()
        }
        .onReceive(NotificationCenter.default.publisher(for: retroAchievementsToastNotification)) { notification in
            presentRetroAchievementsToast(notification)
        }
        .onReceive(Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()) { _ in
            refreshRuntimeMenuState()
        }
        .persistentSystemOverlays(.hidden)
    }

    // MARK: - Layout Views

    @ViewBuilder
    private func menuButtonOverlay(isLandscape: Bool) -> some View {
        if !menuButtonHidden {
            VStack {
                HStack {
                    Spacer()
                    menuButton()
                }
                .padding(.top, isLandscape ? 8 : 4)
                .padding(.trailing, isLandscape ? 8 : 4)
                Spacer()
            }
        }
    }

    private func menuButton() -> some View {
        Menu {
            Button {
                cycleOsdPreset()
            } label: {
                Label(settings.localized("OSD"), systemImage: "speedometer")
            }
            Toggle(isOn: $userVirtualPadVisible) {
                Label(settings.localized("Virtual Pad"), systemImage: "gamecontroller")
            }
            controllerSkinMenu
            if virtualPadHiddenByController {
                Text(settings.localized("Hidden while controller is connected"))
            }
            Toggle(isOn: Binding(
                get: { fullScreen },
                set: { newValue in
                    fullScreen = newValue
                    ARMSX2Bridge.setFullScreen(newValue)
                }
            )) {
                Label(settings.localized("Full Screen"), systemImage: "arrow.up.left.and.arrow.down.right")
            }
            Toggle(isOn: Binding(
                get: { menuButtonHidden || settings.hideMenuButton },
                set: { newValue in
                    settings.hideMenuButton = newValue
                    menuButtonHidden = newValue
                    if newValue {
                        presentStatusMessage(settings.localized("Double-tap empty gameplay space to show the menu button again."))
                    }
                }
            )) {
                Label(settings.localized("Hide Menu Button"), systemImage: "eye.slash")
            }
            Button {
                presentQuickMenuPanel("pad_layout") {
                    showPadLayoutEditor = true
                }
            } label: {
                Label(settings.localized("Edit Virtual Pad Layout"), systemImage: "square.resize")
            }

            Divider()

            Button {
                presentQuickMenuPanel("compatibility_lab") {
                    refreshCompatibilityState()
                    showCompatibilityLab = true
                }
            } label: {
                Label(settings.localized("Compatibility Lab"), systemImage: "wand.and.stars")
            }

            if gameMenuAvailable {
                Button {
                    presentQuickMenuPanel("per_game_settings") {
                        openPerGameSettingsForCurrentGame()
                    }
                } label: {
                    Label(settings.localized("Per-Game Settings"), systemImage: "slider.horizontal.3")
                }
            }

            if vmMenuAvailable {
                Button {
                    presentQuickMenuPanel("speed_control") {
                        showSpeedControl = true
                    }
                } label: {
                    Label(settings.localized("Speed / Fast Forward"), systemImage: "forward.fill")
                }

                Button {
                    presentQuickMenuPanel("reset_rom") {
                        showResetConfirmation = true
                    }
                } label: {
                    Label(settings.localized("Reset ROM"), systemImage: "arrow.counterclockwise.circle")
                }
            }

            if gameMenuAvailable || vmMenuAvailable {
                Button {
                    presentQuickMenuPanel("save_states") {
                        showSaveStates = true
                    }
                } label: {
                    Label(settings.localized("Save / Load States"), systemImage: "square.stack.3d.up.fill")
                }
            }

            if vmMenuAvailable {
                discSwapMenu
            }

            if gameMenuAvailable {
                Button {
                    presentQuickMenuPanel("retroachievements") {
                        showRetroAchievements = true
                    }
                } label: {
                    Label(settings.localized("RetroAchievements"), systemImage: "trophy.fill")
                }

                Button {
                    presentQuickMenuPanel("pnach_import") {
                        showPNACHImporter = true
                    }
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

    private var controllerSkinMenu: some View {
        Menu {
            ForEach(skinLibrary.allDescriptors) { skin in
                Button {
                    skinLibrary.selectSkin(id: skin.id)
                    settings.virtualPadSkin = skin.virtualPadSkin
                    presentStatusMessage("\(settings.localized("Controller Skin")): \(settings.localized(skin.displayName))")
                } label: {
                    Label(settings.localized(skin.displayName), systemImage: skinLibrary.selectedSkinID == skin.id ? "checkmark" : "circle")
                }
            }
        } label: {
            Label(settings.localized("Controller Skin"), systemImage: "paintpalette")
        }
    }

    private func presentQuickMenuPanel(_ name: String, _ action: @escaping () -> Void) {
        NSLog("[ARMSX2 iOS QuickMenu] present \(name)")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) {
            action()
        }
    }

    private var discSwapMenu: some View {
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

    // MARK: - Runtime Panels

    private var compatibilityLabPanel: some View {
        NavigationStack {
            Form {
                compatibilityStatusSection
                compatibilityResetSection
                compatibilityFlagsSection
                if !compatibilityIdentity.isEmpty {
                    compatibilityForgetSection
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

    private var compatibilityStatusSection: some View {
        let currentPreset = compatibilityPreset(for: compatibilityPresetKey)
        return Section {
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
    }

    private var compatibilityResetSection: some View {
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
    }

    private var compatibilityFlagsSection: some View {
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
    }

    private var compatibilityForgetSection: some View {
        Section {
            Button(role: .destructive) {
                let identity = compatibilityIdentity
                ARMSX2Bridge.forgetCompatibilityPresetForCurrentGame()
                refreshCompatibilityState()
                presentStatusMessage("\(settings.localized("Compatibility preset reset for")) \(identity)")
            } label: {
                Label(settings.localized("Forget This Game's Override"), systemImage: "trash")
            }
        }
    }

    @ViewBuilder
    private var runtimePerGameSettingsContent: some View {
        if let runtimePerGameSettingsEntry {
            PerGameSettingsPanel(game: runtimePerGameSettingsEntry, preloadedSettings: runtimePerGameSettings, savesToRunningGame: true) {
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

    // MARK: - Lifecycle & Events

    private func enterGameplaySystemChromeMode() {
        previousHideHomeIndicator = appState.hideHomeIndicator
        appState.hideHomeIndicator = true
    }

    private func leaveGameplaySystemChromeMode() {
        appState.hideHomeIndicator = previousHideHomeIndicator
    }

    private func applyInitialFullscreenPreference() {
        menuButtonHidden = settings.hideMenuButton
        if settings.autoFullscreen && !ARMSX2Bridge.isSDLFullscreen() {
            fullScreen = true
            ARMSX2Bridge.setFullScreen(true)
        }
    }

    private func syncFullscreenStateFromWindow() {
        let sdlFullscreen = ARMSX2Bridge.isSDLFullscreen()
        if fullScreen != sdlFullscreen {
            fullScreen = sdlFullscreen
        }
    }

    private func restoreMenuButtonIfHidden() {
        guard menuButtonHidden else { return }

        menuButtonHidden = false
        settings.hideMenuButton = false
        presentStatusMessage(settings.localized("Menu button shown"))
    }

    private func updateRuntimeOverlayPause() {
        let shouldPause = showSaveStates || showSpeedControl || showCompatibilityLab || showPerGameSettings || showPNACHImporter || showPadLayoutEditor || showResetConfirmation
        NSLog("@@RUNTIME_OVERLAY_PAUSE@@ should=%d active=%d vm=%d", shouldPause ? 1 : 0, runtimeOverlayPauseActive ? 1 : 0, ARMSX2Bridge.isVMRunning() ? 1 : 0)
        guard runtimeOverlayPauseActive != shouldPause else { return }

        runtimeOverlayPauseActive = shouldPause
        if ARMSX2Bridge.isVMRunning() {
            ARMSX2Bridge.setVMPaused(shouldPause)
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
        let identity = runtimePadLayoutIdentityForCurrentGame()
        if runtimePadLayoutIdentity != identity {
            runtimePadLayoutIdentity = identity
        }
        refreshCompatibilityState()
    }

    private func refreshExternalControllerConnectionState() {
        let connected = !GCController.controllers().isEmpty
        if externalControllerConnected != connected {
            externalControllerConnected = connected
        }
    }

    // MARK: - Game Identity Helpers

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

    // MARK: - Compatibility Helpers

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
            presentStatusMessage("\(settings.localized(preset.title)) \(settings.localized("saved for")) \(compatibilityIdentity)")
        } else {
            presentStatusMessage("\(settings.localized("Compatibility preset set to")) \(settings.localized(preset.title))")
        }
    }

    private func compatibilityLabToggle(_ key: String, title: String, systemImage: String) -> some View {
        Toggle(isOn: Binding(
            get: { ARMSX2Bridge.getJITBisectFlag(key, defaultValue: false) },
            set: {
                ARMSX2Bridge.setJITBisectFlag(key, value: $0)
                compatibilityPresetKey = "custom"
                if !compatibilityIdentity.isEmpty {
                    presentStatusMessage("\(settings.localized("Custom compatibility flags saved for")) \(compatibilityIdentity)")
                }
            }
        )) {
            Label(settings.localized(title), systemImage: systemImage)
        }
    }

    // MARK: - Actions

    private func openPerGameSettingsForCurrentGame() {
        // Use the VM-safe bridge path to avoid a disc-image scan while the game is running.
        guard let gameName = currentRuntimeGameName(),
              let info = ARMSX2Bridge.gameSettingsForCurrentGame() else {
            runtimePerGameSettingsEntry = nil
            runtimePerGameSettings = nil
            withAnimation(.spring(response: 0.32, dampingFraction: 0.88)) {
                showPerGameSettings = true
            }
            presentImportantStatusMessage(settings.localized("Per-game settings need a running game."))
            return
        }

        let serial = (info["serial"] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        runtimePerGameSettingsEntry = ISOEntry(
            name: gameName,
            fileURL: nil,
            bootPath: nil,
            coverURL: nil,
            coverSignature: nil,
            metadata: serial.isEmpty ? [:] : ["serial": serial],
            size: 0,
            isFavorite: false
        )
        runtimePerGameSettings = info
        withAnimation(.spring(response: 0.32, dampingFraction: 0.88)) {
            showPerGameSettings = true
        }
    }

    private func closePerGameSettingsOverlay() {
        withAnimation(.easeInOut(duration: 0.2)) {
            showPerGameSettings = false
        }
        runtimePerGameSettingsEntry = nil
        runtimePerGameSettings = nil
        refreshRuntimeMenuState()
    }

    private func resetCurrentROM() {
        appState.resetCurrentVM()
        presentStatusMessage(settings.localized("Restarting ROM..."))
    }

    private func clearCurrentGameCache() {
        guard let gameName = currentRuntimeGameName() else {
            presentImportantStatusMessage(settings.localized("Cache clear needs a running game."))
            return
        }

        let message = ARMSX2Bridge.clearCache(forISO: gameName)
        presentStatusMessage(message)
    }

    private func changeDisc(to discName: String) {
        presentStatusMessage("Changing disc...")
        ARMSX2Bridge.changeDisc(toISO: discName) { success in
            Task { @MainActor in
                if success {
                    presentStatusMessage("\(discName) inserted. Use the game's disc-swap prompt if needed.")
                } else {
                    presentImportantStatusMessage("Could not change discs. Open the game's disc-swap prompt first, or restart with the target disc.")
                }
            }
        }
    }

    private func restartWithDisc(_ discName: String) {
        presentStatusMessage("Restarting with \(discName)...")
        appState.shutdownAndBoot(isoName: discName)
    }

    private func ejectDisc() {
        presentStatusMessage("Ejecting disc...")
        ARMSX2Bridge.ejectDisc { success in
            Task { @MainActor in
                if success {
                    presentStatusMessage("Disc ejected")
                } else {
                    presentImportantStatusMessage("Could not eject the disc. Try again after the game has finished loading.")
                }
            }
        }
    }

    // MARK: - Toast & Feedback

    @ViewBuilder
    private var statusToastOverlay: some View {
        if let statusMessage {
            Text(statusMessage)
                .font(.callout.weight(.semibold))
                .foregroundStyle(.white)
                .padding(.horizontal, 14)
                .padding(.vertical, 10)
                .background(.black.opacity(0.72), in: Capsule())
                .padding(.bottom, 24)
                .padding(.horizontal, max(max(displaySafeAreaInsets.left, displaySafeAreaInsets.right), 14))
                .transition(.opacity.combined(with: .move(edge: .bottom)))
        }
    }

    @ViewBuilder
    private var retroAchievementsToastOverlay: some View {
        if let retroAchievementsToast {
            HStack(spacing: 12) {
                retroAchievementsBadge(path: retroAchievementsToast.badgePath)

                VStack(alignment: .leading, spacing: 2) {
                    Text("RetroAchievements")
                        .font(.caption.weight(.bold))
                        .foregroundStyle(.yellow)
                    Text(retroAchievementsToast.title)
                        .font(.callout.weight(.semibold))
                        .foregroundStyle(.white)
                        .lineLimit(1)
                    if !retroAchievementsToast.message.isEmpty {
                        Text(retroAchievementsToast.message)
                            .font(.caption)
                            .foregroundStyle(.white.opacity(0.82))
                            .lineLimit(2)
                    }
                }

                Spacer(minLength: 0)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 10)
            .frame(maxWidth: 390)
            .background(.black.opacity(0.78), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .stroke(.white.opacity(0.12), lineWidth: 1)
            }
            .padding(.top, max(displaySafeAreaInsets.top, 54))
            .padding(.horizontal, max(max(displaySafeAreaInsets.left, displaySafeAreaInsets.right), 14))
            .allowsHitTesting(false)
            .transition(.opacity.combined(with: .move(edge: .top)))
        }
    }

    @ViewBuilder
    private func retroAchievementsBadge(path: String?) -> some View {
        if let path,
           let image = UIImage(contentsOfFile: path) {
            Image(uiImage: image)
                .resizable()
                .scaledToFit()
                .frame(width: 42, height: 42)
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
        } else {
            Image(systemName: "trophy.fill")
                .font(.system(size: 22, weight: .semibold))
                .foregroundStyle(.yellow)
                .frame(width: 42, height: 42)
                .background(.white.opacity(0.12), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
        }
    }

    private func presentStatusMessage(
        _ message: String,
        displayDuration: TimeInterval = Self.briefStatusDisplayDuration
    ) {
        statusMessageDismissTask?.cancel()
        statusMessageGeneration += 1
        let currentGeneration = statusMessageGeneration
        withAnimation(.easeOut(duration: 0.18)) {
            statusMessage = message
        }
        statusMessageDismissTask = Task { @MainActor in
            try? await Task.sleep(for: .seconds(displayDuration))
            guard !Task.isCancelled else { return }
            guard statusMessageGeneration == currentGeneration else { return }
            withAnimation(.easeIn(duration: 0.18)) {
                statusMessage = nil
            }
        }
    }

    private func cycleOsdPreset() {
        let allPresets: [OsdPreset] = [.off, .simple, .detail, .full]
        guard let currentIndex = allPresets.firstIndex(of: settings.osdPreset) else {
            return
        }
        let nextIndex = (currentIndex + 1) % allPresets.count
        let nextPreset = allPresets[nextIndex]

        settings.osdPreset = nextPreset
        ARMSX2Bridge.setPerformanceOverlayVisible(nextPreset != .off)

        let label = nextPreset != .off
            ? settings.localized(nextPreset.label)
            : settings.localized("OFF")
        presentStatusMessage("OSD: \(label)")
    }

    private func presentRetroAchievementsToast(_ notification: Notification) {
        let title = ((notification.userInfo?["title"] as? String) ?? "")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard !title.isEmpty else { return }

        let message = ((notification.userInfo?["message"] as? String) ?? "")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let badgePathValue = ((notification.userInfo?["badgePath"] as? String) ?? "")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let toast = RetroAchievementsToast(
            title: title,
            message: message,
            badgePath: badgePathValue.isEmpty ? nil : badgePathValue
        )

        retroAchievementsToastDismissTask?.cancel()
        retroAchievementsToastGeneration += 1
        let currentGeneration = retroAchievementsToastGeneration
        withAnimation(.easeOut(duration: 0.18)) {
            retroAchievementsToast = toast
        }
        retroAchievementsToastDismissTask = Task { @MainActor in
            try? await Task.sleep(for: .seconds(Self.retroAchievementsToastDisplayDuration))
            guard !Task.isCancelled else { return }
            guard retroAchievementsToastGeneration == currentGeneration else { return }
            withAnimation(.easeIn(duration: 0.18)) {
                retroAchievementsToast = nil
            }
        }
    }

    private func presentImportantStatusMessage(_ message: String) {
        presentStatusMessage(message, displayDuration: Self.importantStatusDisplayDuration)
    }

    private func presentPNACHImportResult(_ message: String) {
        if Self.isPNACHImportSuccessMessage(message) {
            presentStatusMessage(message)
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

    // MARK: - Virtual Pad

    private var effectiveVirtualPadVisible: Bool {
        userVirtualPadVisible && (!settings.autoHideVirtualPadWhenControllerConnected || !externalControllerConnected) && !showPadLayoutEditor
    }

    private var effectivePadLayoutSnapshot: PadLayoutSnapshot? {
        layoutPresets.effectiveSnapshot(for: runtimePadLayoutIdentity)
    }

    private var effectivePadSkinDescriptor: VPadSkinDescriptor {
        layoutPresets.effectiveSkinDescriptor(for: runtimePadLayoutIdentity, using: skinLibrary)
    }

    private var runtimePadLayoutEditorContext: PadLayoutEditorContext {
        let preset = layoutPresets.effectivePreset(for: runtimePadLayoutIdentity)
        let editablePresetID = runtimePadLayoutIdentity.flatMap { layoutPresets.presetID(for: $0) }
            ?? (runtimePadLayoutIdentity == nil ? layoutPresets.globalPresetID : nil)
        return PadLayoutEditorContext(
            presetID: editablePresetID,
            gameIdentity: runtimePadLayoutIdentity,
            initialSnapshot: preset?.snapshot,
            skinDescriptor: effectivePadSkinDescriptor
        )
    }

    private func runtimePadLayoutIdentityForCurrentGame() -> PadLayoutGameIdentity? {
        guard let info = ARMSX2Bridge.gameSettingsForCurrentGame() else {
            return nil
        }
        return PadLayoutGameIdentity(
            serial: info["serial"] as? String,
            crc: info["crc"] as? String
        )
    }

    private var virtualPadHiddenByController: Bool {
        userVirtualPadVisible && settings.autoHideVirtualPadWhenControllerConnected && externalControllerConnected
    }

    // MARK: - Disc Helpers

    private var availableDiscSwapNames: [String] {
        ARMSX2Bridge.availableISOs().filter { !$0.lowercased().hasSuffix(".elf") }
    }
}

// MARK: - RetroAchievements Panel

private struct RetroAchievementsGamePanel: View {
    @Environment(\.dismiss) private var dismiss
    let settings: SettingsStore

    @State private var entries: [RetroAchievementEntry] = []
    @State private var state: [String: Any] = [:]

    var body: some View {
        NavigationStack {
            List {
                summarySection

                if entries.isEmpty {
                    emptySection
                } else {
                    ForEach(groupedEntries, id: \.title) { group in
                        if !group.entries.isEmpty {
                            Section(group.title) {
                                ForEach(group.entries) { entry in
                                    RetroAchievementRow(entry: entry, settings: settings)
                                }
                            }
                        }
                    }
                }
            }
            .navigationTitle(settings.localized("RetroAchievements"))
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button(settings.localized("Done")) {
                        dismiss()
                    }
                }
            }
            .onAppear(perform: refresh)
            .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ARMSX2RetroAchievementsStateChanged"))) { _ in
                refresh()
            }
            .onReceive(NotificationCenter.default.publisher(for: retroAchievementsToastNotification)) { _ in
                refresh()
            }
        }
    }

    private var summarySection: some View {
        Section {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 12) {
                    gameBadge

                    VStack(alignment: .leading, spacing: 4) {
                        Text(gameTitle)
                            .font(.headline)
                            .lineLimit(2)
                        Text(progressText)
                            .font(.subheadline.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }
                }

                ProgressView(value: progressFraction)
                    .tint(.yellow)

                if !richPresence.isEmpty {
                    Label(richPresence, systemImage: "quote.bubble")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .padding(.vertical, 4)
        }
    }

    @ViewBuilder
    private var emptySection: some View {
        Section {
            VStack(spacing: 10) {
                Image(systemName: "trophy")
                    .font(.largeTitle)
                    .foregroundStyle(.secondary)
                Text(emptyTitle)
                    .font(.headline)
                    .multilineTextAlignment(.center)
                Text(emptySubtitle)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
            .frame(maxWidth: .infinity)
            .padding(28)
        }
    }

    @ViewBuilder
    private var gameBadge: some View {
        if let path = state["gameIconPath"] as? String,
           !path.isEmpty,
           let image = UIImage(contentsOfFile: path) {
            Image(uiImage: image)
                .resizable()
                .scaledToFit()
                .frame(width: 54, height: 54)
                .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
        } else {
            Image(systemName: "trophy.fill")
                .font(.system(size: 26, weight: .semibold))
                .foregroundStyle(.yellow)
                .frame(width: 54, height: 54)
                .background(.yellow.opacity(0.14), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
        }
    }

    private var gameTitle: String {
        let title = state["gameTitle"] as? String ?? ""
        return title.isEmpty ? settings.localized("Current Game") : title
    }

    private var richPresence: String {
        state["richPresence"] as? String ?? ""
    }

    private var unlockedCount: Int {
        (state["unlockedAchievements"] as? NSNumber)?.intValue ?? entries.filter(\.isUnlocked).count
    }

    private var totalCount: Int {
        (state["totalAchievements"] as? NSNumber)?.intValue ?? entries.filter { !$0.isUnofficial }.count
    }

    private var unlockedPoints: Int {
        (state["unlockedPoints"] as? NSNumber)?.intValue ?? entries.filter(\.isUnlocked).reduce(0) { $0 + $1.points }
    }

    private var totalPoints: Int {
        (state["totalPoints"] as? NSNumber)?.intValue ?? entries.filter { !$0.isUnofficial }.reduce(0) { $0 + $1.points }
    }

    private var progressFraction: Double {
        guard totalCount > 0 else { return 0 }
        return min(1, max(0, Double(unlockedCount) / Double(totalCount)))
    }

    private var progressText: String {
        "\(unlockedCount)/\(totalCount) \(settings.localized("achievements")) · \(unlockedPoints)/\(totalPoints) \(settings.localized("points"))"
    }

    private var emptyTitle: String {
        if (state["loggedIn"] as? NSNumber)?.boolValue == false {
            return settings.localized("RetroAchievements is not logged in.")
        }
        if (state["hasActiveGame"] as? NSNumber)?.boolValue == false {
            return settings.localized("No RetroAchievements game is active.")
        }
        return settings.localized("No achievements found for this game.")
    }

    private var emptySubtitle: String {
        settings.localized("Start a supported game with RetroAchievements enabled, then reopen this panel.")
    }

    private var groupedEntries: [(title: String, entries: [RetroAchievementEntry])] {
        let active = entries.filter { $0.isActiveChallenge && !$0.isUnlocked && !$0.isUnsupported && !$0.isUnofficial }
        let locked = entries.filter { !$0.isUnlocked && !$0.isActiveChallenge && !$0.isUnsupported && !$0.isUnofficial }
        let unlocked = entries.filter { $0.isUnlocked && !$0.isUnofficial }
        let unofficial = entries.filter(\.isUnofficial)
        let unsupported = entries.filter(\.isUnsupported)

        return [
            (settings.localized("Active / Almost There"), active),
            (settings.localized("Locked"), locked),
            (settings.localized("Unlocked"), unlocked),
            (settings.localized("Unofficial"), unofficial),
            (settings.localized("Unsupported"), unsupported),
        ]
    }

    private func refresh() {
        state = ARMSX2Bridge.retroAchievementsState()
        entries = ARMSX2Bridge.retroAchievementsForCurrentGame()
            .compactMap { RetroAchievementEntry(dictionary: $0) }
    }
}

private struct RetroAchievementRow: View {
    let entry: RetroAchievementEntry
    let settings: SettingsStore

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            badge

            VStack(alignment: .leading, spacing: 5) {
                HStack(alignment: .firstTextBaseline, spacing: 8) {
                    Text(entry.title.isEmpty ? settings.localized("Untitled Achievement") : entry.title)
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(entry.isUnlocked ? .primary : .secondary)
                        .lineLimit(2)
                    Spacer(minLength: 8)
                    Text("\(entry.points) pts")
                        .font(.caption.monospacedDigit().weight(.semibold))
                        .foregroundStyle(.yellow)
                }

                if !entry.description.isEmpty {
                    Text(entry.description)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(3)
                }

                HStack(spacing: 8) {
                    Label(statusText, systemImage: statusIcon)
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(statusColor)

                    if !entry.measuredProgress.isEmpty {
                        Text(entry.measuredProgress)
                            .font(.caption2.monospacedDigit())
                            .foregroundStyle(.secondary)
                    } else if entry.measuredPercent > 0 && entry.measuredPercent < 100 {
                        Text("\(Int(entry.measuredPercent.rounded()))%")
                            .font(.caption2.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }

                    Spacer(minLength: 0)
                }
            }
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder
    private var badge: some View {
        if let path = entry.badgePath,
           let image = UIImage(contentsOfFile: path) {
            Image(uiImage: image)
                .resizable()
                .scaledToFit()
                .frame(width: 46, height: 46)
                .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
                .opacity(entry.isUnlocked ? 1 : 0.55)
        } else {
            Image(systemName: entry.isUnlocked ? "trophy.fill" : "lock.fill")
                .font(.system(size: 22, weight: .semibold))
                .foregroundStyle(entry.isUnlocked ? .yellow : .secondary)
                .frame(width: 46, height: 46)
                .background(.secondary.opacity(0.12), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
        }
    }

    private var statusText: String {
        if entry.isUnsupported { return settings.localized("Unsupported") }
        if entry.isUnofficial { return settings.localized("Unofficial") }
        if entry.isUnlocked { return settings.localized("Unlocked") }
        if entry.isActiveChallenge { return settings.localized("Active") }
        return settings.localized("Locked")
    }

    private var statusIcon: String {
        if entry.isUnsupported { return "exclamationmark.triangle.fill" }
        if entry.isUnofficial { return "sparkles" }
        if entry.isUnlocked { return "checkmark.seal.fill" }
        if entry.isActiveChallenge { return "flame.fill" }
        return "lock.fill"
    }

    private var statusColor: Color {
        if entry.isUnsupported { return .orange }
        if entry.isUnofficial { return .purple }
        if entry.isUnlocked { return .green }
        if entry.isActiveChallenge { return .red }
        return .secondary
    }
}

// MARK: - Save States Panel

private struct SaveStatesPanel: View {
    @Environment(\.dismiss) private var dismiss
    @State private var settings = SettingsStore.shared
    @State private var slots: [ARMSX2SaveStateSlotInfo] = []
    @State private var busySlot: Int? = nil
    @State private var pendingOverwrite: ARMSX2SaveStateSlotInfo? = nil
    @State private var hardcoreActive = false

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
                                onOverwrite: { pendingOverwrite = slot },
                                loadDisabled: hardcoreActive,
                                settings: settings
                            )
                        }
                    }
                    .padding()
                }
            }
            .safeAreaInset(edge: .top) {
                Text(settings.localized(hardcoreActive ?
                    "Hardcore mode allows saving states for debugging, but loading states is blocked." :
                    "Empty slots can save. Occupied slots can load or overwrite."))
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
            .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ARMSX2RetroAchievementsStateChanged"))) { _ in
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
        hardcoreActive = ARMSX2Bridge.isRetroAchievementsHardcoreActive()
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
        guard !hardcoreActive else {
            statusHandler(settings.localized("Hardcore mode blocks loading save states."), true)
            return
        }

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

// MARK: - Speed Control Panel

private struct SpeedControlPanel: View {
    @Bindable var settings: SettingsStore
    @Environment(\.dismiss) private var dismiss
    @State private var hardcoreActive = false

    var body: some View {
        NavigationStack {
            Form {
                Section(settings.localized("Fast Forward")) {
                    Toggle(settings.localized("Enable Fast Forward"), isOn: Binding(
                        get: { settings.fastForwardRuntimeEnabled },
                        set: { enabled in
                            settings.setRuntimeFastForwardEnabled(enabled)
                        }
                    ))

                    VStack(alignment: .leading, spacing: 10) {
                        HStack {
                            Text(settings.localized("Fast Forward Speed"))
                            Spacer()
                            Text(Self.formatPercent(settings.fastForwardScalar))
                                .foregroundStyle(.secondary)
                                .font(.callout.monospacedDigit())
                        }

                        Slider(
                            value: $settings.fastForwardScalar,
                            in: SettingsStore.minFastForwardScalar...SettingsStore.maxFastForwardScalar,
                            step: 0.25
                        )

                        HStack {
                            quickFastForwardButton(1.5)
                            quickFastForwardButton(2.0)
                            quickFastForwardButton(3.0)
                            quickFastForwardButton(5.0)
                            quickFastForwardButton(10.0)
                        }
                    }
                }

                Section(settings.localized("Frame Limiter")) {
                    Toggle(settings.localized("Enable Limiter"), isOn: Binding(
                        get: { settings.frameLimiterEnabled },
                        set: { enabled in
                            settings.frameLimiterEnabled = enabled
                            enforceHardcoreSpeedFloorIfNeeded()
                        }
                    ))

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
                                value: Binding(
                                    get: { settings.targetFPS },
                                    set: { value in
                                        settings.targetFPS = value
                                        enforceHardcoreSpeedFloorIfNeeded()
                                    }
                                ),
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

                if hardcoreActive {
                    Section(settings.localized("Hardcore Mode")) {
                        Text(settings.localized("Hardcore mode blocks slowdown and frame advance. Fast forward stays available; Normal Speed is locked at 100% or higher."))
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
            .navigationTitle(settings.localized("Speed / Fast Forward"))
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button(settings.localized("Done")) {
                        dismiss()
                    }
                }
            }
            .onAppear {
                refreshRuntimeState()
            }
            .onReceive(NotificationCenter.default.publisher(for: Notification.Name("ARMSX2RetroAchievementsStateChanged"))) { _ in
                refreshRuntimeState()
            }
        }
    }

    private func refreshRuntimeState() {
        settings.fastForwardRuntimeEnabled = ARMSX2Bridge.limiterMode() == 1
        hardcoreActive = ARMSX2Bridge.isRetroAchievementsHardcoreActive()
        enforceHardcoreSpeedFloorIfNeeded()
    }

    private func enforceHardcoreSpeedFloorIfNeeded() {
        guard hardcoreActive else { return }
        let minimumFPS = settings.ntscFramerate
        if settings.frameLimiterEnabled && settings.targetFPS < minimumFPS {
            settings.targetFPS = minimumFPS
        }
    }

    private func quickFastForwardButton(_ scalar: Float) -> some View {
        Button(Self.formatPercent(scalar)) {
            settings.fastForwardScalar = scalar
        }
        .buttonStyle(.bordered)
        .font(.caption.monospacedDigit())
    }

    private func quickTargetButton(_ fps: Float) -> some View {
        Button(Self.formatCompactFPS(fps)) {
            settings.frameLimiterEnabled = true
            settings.targetFPS = fps
            enforceHardcoreSpeedFloorIfNeeded()
        }
        .disabled(hardcoreActive && fps < settings.ntscFramerate)
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

// MARK: - Save State Slot Row

private struct SaveStateSlotRow: View {
    let info: ARMSX2SaveStateSlotInfo
    let isBusy: Bool
    let onSave: () -> Void
    let onLoad: () -> Void
    let onOverwrite: () -> Void
    let loadDisabled: Bool
    let settings: SettingsStore

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 12) {
                SaveStatePreview(data: info.previewPNGData, occupied: info.occupied)
                    .frame(width: 96, height: 72)

                VStack(alignment: .leading, spacing: 4) {
                    Text("\(settings.localized("Slot")) \(info.slot)")
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
                        Text(settings.localized("Empty"))
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
                        Label(settings.localized("Save"), systemImage: "square.and.arrow.down")
                    }
                    .buttonStyle(.borderedProminent)
                }
            }

            if info.occupied && !isBusy {
                HStack(spacing: 8) {
                    Button(action: onLoad) {
                        Label(settings.localized("Load"), systemImage: "arrow.down.circle")
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(loadDisabled)
                    .frame(maxWidth: .infinity)

                    Button(action: onOverwrite) {
                        Label(settings.localized("Overwrite"), systemImage: "arrow.triangle.2.circlepath")
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

// MARK: - Save State Preview

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
