// MemoryCardSettingsView.swift — iOS memory card creation and slot assignment
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI

struct MemoryCardSettingsView: View {
    @State private var settings = SettingsStore.shared
    @State private var availableCards: [String] = []
    @State private var slot1Card = ""
    @State private var slot2Card = ""
    @State private var newCardName = "Mcd003"
    @State private var newCardSizeMB = 8
    @State private var createFolderCard = false
    @State private var resultMessage: String?
    @State private var showResult = false

    private let cardSizes = [8, 16, 32, 64]
    private let pathLikeCharacters: [Character] = ["/", "\\", ":", "*", "?", "\"", "<", ">", "|"]

    var body: some View {
        Form {
            Section(settings.localized("Directory")) {
                Text(ARMSX2Bridge.memoryCardDirectory())
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }

            Section(settings.localized("Assigned Cards")) {
                Picker(settings.localized("Slot 1"), selection: $slot1Card) {
                    Text(settings.localized("Unplugged")).tag("")
                    ForEach(availableCards, id: \.self) { card in
                        Text(card).tag(card)
                    }
                }
                .onChange(of: slot1Card) { _, newValue in
                    ARMSX2Bridge.setMemoryCard(name: newValue, forSlot: 1, enabled: !newValue.isEmpty)
                }

                Picker(settings.localized("Slot 2"), selection: $slot2Card) {
                    Text(settings.localized("Unplugged")).tag("")
                    ForEach(availableCards, id: \.self) { card in
                        Text(card).tag(card)
                    }
                }
                .onChange(of: slot2Card) { _, newValue in
                    ARMSX2Bridge.setMemoryCard(name: newValue, forSlot: 2, enabled: !newValue.isEmpty)
                }

                Text(settings.localized("Slot changes take effect on the next VM boot."))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section {
                TextField(settings.localized("Card name"), text: $newCardName)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()

                Toggle(settings.localized("Folder Memory Card"), isOn: $createFolderCard)

                if !createFolderCard {
                    Picker(settings.localized("Size"), selection: $newCardSizeMB) {
                        ForEach(cardSizes, id: \.self) { size in
                            Text("\(size) MB").tag(size)
                        }
                    }
                }

                Button {
                    createCard()
                } label: {
                    Label(settings.localized(createFolderCard ? "Create Folder Card" : "Create Card"), systemImage: "memorychip")
                }
            } header: {
                Text(settings.localized("Create Memory Card"))
            } footer: {
                Text(settings.localized("File cards support 8 MB, 16 MB, 32 MB, and 64 MB. Folder cards match the ARMSX2/PCSX2 folder-card behavior and are useful for game-specific saves."))
            }

            Section(settings.localized("Available Cards")) {
                if availableCards.isEmpty {
                    Text(settings.localized("No cards found."))
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(availableCards, id: \.self) { card in
                        Text(card)
                    }
                }
            }
        }
        .navigationTitle(settings.localized("Memory Cards"))
        .navigationBarTitleDisplayMode(.inline)
        .onAppear(perform: refresh)
        .alert(settings.localized("Memory Cards"), isPresented: $showResult) {
            Button(settings.localized("OK")) {}
        } message: {
            Text(settings.localized(resultMessage ?? ""))
        }
    }

    private func refresh() {
        availableCards = ARMSX2Bridge.availableMemoryCards()
        slot1Card = ARMSX2Bridge.memoryCardName(forSlot: 1) ?? ""
        slot2Card = ARMSX2Bridge.memoryCardName(forSlot: 2) ?? ""
    }

    private func createCard() {
        if let validationMessage = validateNewMemoryCardName(newCardName) {
            resultMessage = validationMessage
            showResult = true
            return
        }

        let success = ARMSX2Bridge.createMemoryCard(named: newCardName, sizeMB: newCardSizeMB, folder: createFolderCard)
        refresh()
        resultMessage = success ? "Memory card created." : "Could not create memory card. Check the name, size, or whether it already exists."
        showResult = true
    }

    private func validateNewMemoryCardName(_ name: String) -> String? {
        let trimmedName = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmedName.isEmpty {
            return "Enter a name for the memory card first."
        }

        if trimmedName.contains(where: { pathLikeCharacters.contains($0) }) {
            return "Memory card names cannot contain folder or path characters like / or \\."
        }

        if availableCards.contains(where: { $0.caseInsensitiveCompare(trimmedName) == .orderedSame }) {
            return "A memory card with this name already exists."
        }

        return nil
    }
}
