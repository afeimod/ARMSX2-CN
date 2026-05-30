// GameScreenView.swift — Unified game screen (Metal + Virtual Pad + Menu)
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

private let runtimeMenuStateChangedNotification = Notification.Name("ARMSX2iOSRuntimeMenuStateChanged")

struct GameScreenView: View {
    @State private var appState = AppState.shared
    @State private var settings = SettingsStore.shared
    @State private var fileImporter = FileImportHandler.shared
    @State private var padVisible = true
    @State private var fullScreen = false
    @State private var vmMenuAvailable = false
    @State private var gameMenuAvailable = false
    @State private var showSaveStates = false
    @State private var showPNACHImporter = false
    @State private var saveStateStatus: String? = nil

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
                // Portrait: Metal top half, pad bottom half, within safe area
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
        .onChange(of: fullScreen) { _, newValue in
            ARMSX2Bridge.setFullScreen(newValue)
        }
        .sheet(isPresented: $showSaveStates) {
            SaveStatesPanel { message in
                presentSaveStateStatus(message)
            }
        }
        .fileImporter(
            isPresented: $showPNACHImporter,
            allowedContentTypes: FileImportHandler.pnachContentTypes,
            allowsMultipleSelection: true
        ) { result in
            switch result {
            case .success(let urls):
                let message = fileImporter.importPNACHURLs(urls, asCheat: true)
                presentSaveStateStatus(message)
            case .failure(let error):
                presentSaveStateStatus("PNACH import failed: \(error.localizedDescription)")
            }
        }
        .overlay(alignment: .bottom) {
            saveStateToast
        }
        .onAppear(perform: refreshRuntimeMenuState)
        .onReceive(NotificationCenter.default.publisher(for: runtimeMenuStateChangedNotification)) { _ in
            refreshRuntimeMenuState()
        }
        .onReceive(Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()) { _ in
            refreshRuntimeMenuState()
        }
    }

    private func menuOverlay(isLandscape: Bool) -> some View {
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
                Label("OSD", systemImage: "speedometer")
            }
            Toggle(isOn: $padVisible) {
                Label("Virtual Pad", systemImage: "gamecontroller")
            }
            if isLandscape {
                Toggle(isOn: $fullScreen) {
                    Label("Full Screen", systemImage: "arrow.up.left.and.arrow.down.right")
                }
            }
            Divider()
            compatibilityLabSection
            if vmMenuAvailable {
                Menu {
                    Button {
                        ejectDisc()
                    } label: {
                        Label("Eject Disc", systemImage: "eject")
                    }

                    let discs = availableDiscSwapNames
                    if discs.isEmpty {
                        Text("No disc images found")
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
                            Label("Insert Disc (No Reboot)", systemImage: "tray.and.arrow.down")
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
                            Label("Restart With Disc", systemImage: "arrow.clockwise.circle")
                        }
                    }
                } label: {
                    Label("Change Disc", systemImage: "opticaldisc")
                }
            }

            if gameMenuAvailable {
                Button {
                    showPNACHImporter = true
                } label: {
                    Label("Import PNACH Cheat", systemImage: "wand.and.stars")
                }

                Button {
                    showSaveStates = true
                } label: {
                    Label("Save / Load States", systemImage: "square.stack.3d.up.fill")
                }
            } else if vmMenuAvailable {
                Button {
                    showSaveStates = true
                } label: {
                    Label("Save / Load States", systemImage: "square.stack.3d.up.fill")
                }
            }
            Divider()
            Button {
                appState.returnToMenu()
            } label: {
                Label("Back to Menu", systemImage: "list.bullet")
            }
        } label: {
            Image(systemName: "ellipsis.circle.fill")
                .font(.title3)
                .foregroundStyle(.white.opacity(0.5))
                .padding(6)
                .background(.black.opacity(0.15), in: Circle())
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
    }

    private var compatibilityLabSection: some View {
        Section("Compatibility Lab") {
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
        }
    }

    private func compatibilityLabToggle(_ key: String, title: String, systemImage: String) -> some View {
        Toggle(isOn: Binding(
            get: { ARMSX2Bridge.getJITBisectFlag(key, defaultValue: false) },
            set: { ARMSX2Bridge.setJITBisectFlag(key, value: $0) }
        )) {
            Label(title, systemImage: systemImage)
        }
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

    private func presentSaveStateStatus(_ message: String) {
        withAnimation(.easeOut(duration: 0.18)) {
            saveStateStatus = message
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.2) {
            guard saveStateStatus == message else { return }
            withAnimation(.easeIn(duration: 0.18)) {
                saveStateStatus = nil
            }
        }
    }

    private var availableDiscSwapNames: [String] {
        ARMSX2Bridge.availableISOs().filter { !$0.lowercased().hasSuffix(".elf") }
    }

    private func changeDisc(to discName: String) {
        presentSaveStateStatus("Changing disc...")
        ARMSX2Bridge.changeDisc(toISO: discName) { success in
            Task { @MainActor in
                presentSaveStateStatus(success ? "\(discName) inserted. Use the game's disc-swap prompt if needed." : "Failed to change disc")
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
                presentSaveStateStatus(success ? "Disc ejected" : "Failed to eject disc")
            }
        }
    }
}

private struct SaveStatesPanel: View {
    @Environment(\.dismiss) private var dismiss
    @State private var slots: [ARMSX2SaveStateSlotInfo] = []
    @State private var busySlot: Int? = nil
    @State private var pendingOverwrite: ARMSX2SaveStateSlotInfo? = nil

    let statusHandler: (String) -> Void

    var body: some View {
        NavigationStack {
            ScrollView {
                if slots.isEmpty {
                    VStack(spacing: 10) {
                        Image(systemName: "hourglass")
                            .font(.largeTitle)
                            .foregroundStyle(.secondary)
                        Text("Save states are not ready yet.")
                            .font(.headline)
                        Text("Wait until the game has fully identified, then try again.")
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
                Text("Empty slots can save. Occupied slots can load or overwrite.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal)
                    .padding(.vertical, 8)
                    .background(.regularMaterial)
            }
            .navigationTitle("Save / Load States")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Done") {
                        dismiss()
                    }
                }
            }
            .onAppear(perform: refresh)
            .onReceive(NotificationCenter.default.publisher(for: runtimeMenuStateChangedNotification)) { _ in
                refresh()
            }
            .confirmationDialog(
                "Overwrite Slot \(pendingOverwrite?.slot ?? 0)?",
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
                Button("Overwrite", role: .destructive) {
                    if let pendingOverwrite {
                        save(pendingOverwrite)
                    }
                    pendingOverwrite = nil
                }
                Button("Cancel", role: .cancel) {
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
                statusHandler(success ? "State saved to slot \(slotNumber)" : "Failed to save slot \(slotNumber)")
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
                statusHandler(success ? "State loaded from slot \(slotNumber)" : "Failed to load slot \(slotNumber)")
                if success {
                    dismiss()
                }
            }
        }
    }
}

private struct SaveStateSlotRow: View {
    let info: ARMSX2SaveStateSlotInfo
    let isBusy: Bool
    let onSave: () -> Void
    let onLoad: () -> Void
    let onOverwrite: () -> Void

    var body: some View {
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
            } else if info.occupied {
                HStack(spacing: 8) {
                    Button(action: onLoad) {
                        Label("Load", systemImage: "arrow.down.circle")
                    }
                    .buttonStyle(.borderedProminent)

                    Button(action: onOverwrite) {
                        Label("Overwrite", systemImage: "arrow.triangle.2.circlepath")
                    }
                    .buttonStyle(.bordered)
                }
            } else {
                Button(action: onSave) {
                    Label("Save", systemImage: "square.and.arrow.down")
                }
                .buttonStyle(.borderedProminent)
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
