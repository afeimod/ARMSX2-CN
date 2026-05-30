// MemoryCardSettingsView.swift — iOS memory card creation and slot assignment
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct MemoryCardSettingsView: View {
    @State private var availableCards: [String] = []
    @State private var slot1Card = ""
    @State private var slot2Card = ""
    @State private var newCardName = "Mcd003"
    @State private var newCardSizeMB = 8
    @State private var createFolderCard = false
    @State private var resultMessage: String?
    @State private var showResult = false

    private let cardSizes = [8, 16, 32, 64]

    var body: some View {
        Form {
            Section("Directory") {
                Text(ARMSX2Bridge.memoryCardDirectory())
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }

            Section("Assigned Cards") {
                Picker("Slot 1", selection: $slot1Card) {
                    Text("Unplugged").tag("")
                    ForEach(availableCards, id: \.self) { card in
                        Text(card).tag(card)
                    }
                }
                .onChange(of: slot1Card) { _, newValue in
                    ARMSX2Bridge.setMemoryCard(name: newValue, forSlot: 1, enabled: !newValue.isEmpty)
                }

                Picker("Slot 2", selection: $slot2Card) {
                    Text("Unplugged").tag("")
                    ForEach(availableCards, id: \.self) { card in
                        Text(card).tag(card)
                    }
                }
                .onChange(of: slot2Card) { _, newValue in
                    ARMSX2Bridge.setMemoryCard(name: newValue, forSlot: 2, enabled: !newValue.isEmpty)
                }

                Text("Slot changes take effect on the next VM boot.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                TextField("Card name", text: $newCardName)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()

                Toggle("Folder Memory Card", isOn: $createFolderCard)

                if !createFolderCard {
                    Picker("Size", selection: $newCardSizeMB) {
                        ForEach(cardSizes, id: \.self) { size in
                            Text("\(size) MB").tag(size)
                        }
                    }
                }

                Button {
                    createCard()
                } label: {
                    Label(createFolderCard ? "Create Folder Card" : "Create Card", systemImage: "memorychip")
                }
            } header: {
                Text("Create Memory Card")
            } footer: {
                Text("File cards support 8 MB, 16 MB, 32 MB, and 64 MB. Folder cards match the ARMSX2/PCSX2 folder-card behavior and are useful for game-specific saves.")
            }

            Section("Available Cards") {
                if availableCards.isEmpty {
                    Text("No cards found.")
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(availableCards, id: \.self) { card in
                        Text(card)
                    }
                }
            }
        }
        .navigationTitle("Memory Cards")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear(perform: refresh)
        .alert("Memory Cards", isPresented: $showResult) {
            Button("OK") {}
        } message: {
            Text(resultMessage ?? "")
        }
    }

    private func refresh() {
        availableCards = ARMSX2Bridge.availableMemoryCards()
        slot1Card = ARMSX2Bridge.memoryCardName(forSlot: 1) ?? ""
        slot2Card = ARMSX2Bridge.memoryCardName(forSlot: 2) ?? ""
    }

    private func createCard() {
        let success = ARMSX2Bridge.createMemoryCard(named: newCardName, sizeMB: newCardSizeMB, folder: createFolderCard)
        refresh()
        resultMessage = success ? "Memory card created." : "Could not create memory card. Check the name, size, or whether it already exists."
        showResult = true
    }
}
