// BIOSListView.swift — BIOS file list with default selection
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers

struct BIOSListView: View {
    @State private var bioses: [String] = []
    @State private var defaultBIOS: String = ""
    @State private var fileImporter = FileImportHandler.shared
    @State private var showBIOSImporter = false

    var body: some View {
        NavigationStack {
            Group {
                if bioses.isEmpty {
                    emptyState
                } else {
                    List {
                        ForEach(bioses, id: \.self) { bios in
                            biosRow(bios)
                        }
                    }
#if targetEnvironment(macCatalyst)
                    .listStyle(.inset)
#endif
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .navigationTitle("BIOS")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button { showBIOSImporter = true } label: {
                        Image(systemName: "plus")
                    }
                    .accessibilityLabel("Import BIOS")
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button { loadBIOSes() } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                }
            }
            .fileImporter(
                isPresented: $showBIOSImporter,
                allowedContentTypes: FileImportHandler.biosContentTypes,
                allowsMultipleSelection: true
            ) { result in
                switch result {
                case .success(let urls):
                    fileImporter.handleURLs(urls, preferredDestination: .bios)
                    loadBIOSes()
                    if defaultBIOS.isEmpty, let firstBIOS = bioses.first {
                        ARMSX2Bridge.setDefaultBIOS(firstBIOS)
                        defaultBIOS = firstBIOS
                    }
                case .failure(let error):
                    fileImporter.lastImportMessage = "Import failed: \(error.localizedDescription)"
                    fileImporter.showImportAlert = true
                }
            }
        }
        .onAppear { loadBIOSes() }
    }

    private func biosRow(_ bios: String) -> some View {
        Button {
            ARMSX2Bridge.setDefaultBIOS(bios)
            defaultBIOS = bios
        } label: {
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text(bios)
                        .font(.body)
                        .foregroundStyle(.primary)
                    Text(regionGuess(bios))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                if bios == defaultBIOS {
                    Image(systemName: "checkmark.circle.fill")
                        .foregroundStyle(.blue)
                }
            }
        }
        .foregroundStyle(.primary)
    }

    private var emptyState: some View {
        VStack(spacing: 16) {
            Image(systemName: "cpu")
                .font(.system(size: 48))
                .foregroundStyle(.secondary)
            Text("No BIOS Found")
                .font(.title2)
                .fontWeight(.semibold)
            Text("Import a PS2 BIOS dump to enable booting.")
                .font(.body)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Button {
                showBIOSImporter = true
            } label: {
                Label("Import BIOS", systemImage: "plus")
            }
            .buttonStyle(.borderedProminent)
        }
        .padding()
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func loadBIOSes() {
        bioses = ARMSX2Bridge.availableBIOSes()
        defaultBIOS = ARMSX2Bridge.defaultBIOSName()
    }

    private func regionGuess(_ name: String) -> String {
        let upper = name.uppercased()
        if upper.contains("JP") || upper.contains("JAPAN") || upper.contains("70000") || upper.contains("50000") {
            return "Japan"
        } else if upper.contains("US") || upper.contains("AMERICA") || upper.contains("30001") || upper.contains("39001") {
            return "North America"
        } else if upper.contains("EU") || upper.contains("EUROPE") || upper.contains("30004") || upper.contains("39004") {
            return "Europe"
        }
        return "Unknown Region"
    }
}
