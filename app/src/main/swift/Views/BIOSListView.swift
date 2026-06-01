// BIOSListView.swift — BIOS file list with default selection
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UniformTypeIdentifiers

struct BIOSListView: View {
    @State private var bioses: [ARMSX2BIOSInfo] = []
    @State private var defaultBIOS: String = ""
    @State private var settings = SettingsStore.shared
    @State private var fileImporter = FileImportHandler.shared
    @State private var showBIOSImporter = false
    @State private var showBIOSCompatibilityImporter = false

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
            .navigationTitle(settings.localized("BIOS"))
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Button {
                            NSLog("[ARMSX2 iOS BIOS] opening primary BIOS picker")
                            showBIOSImporter = true
                        } label: {
                            Label(settings.localized("Import BIOS"), systemImage: "doc.badge.plus")
                        }
                        Button {
                            NSLog("[ARMSX2 iOS BIOS] opening compatibility BIOS picker")
                            showBIOSCompatibilityImporter = true
                        } label: {
                            Label(settings.localized("Compatibility Picker"), systemImage: "folder.badge.plus")
                        }
                    } label: {
                        Image(systemName: "plus")
                    }
                    .accessibilityLabel(settings.localized("Import BIOS"))
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button { loadBIOSes() } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                }
            }
            .alert(settings.localized("Import Result"), isPresented: $fileImporter.showImportAlert) {
                Button(settings.localized("OK")) {}
            } message: {
                Text(fileImporter.lastImportMessage ?? "")
            }
            .sheet(isPresented: $showBIOSImporter) {
                ImportDocumentPicker(
                    allowedContentTypes: FileImportHandler.biosContentTypes,
                    allowsMultipleSelection: false
                ) { result in
                    showBIOSImporter = false
                    handleBIOSPickerResult(result, source: "primary")
                }
            }
            .sheet(isPresented: $showBIOSCompatibilityImporter) {
                ImportDocumentPicker(
                    allowedContentTypes: FileImportHandler.biosContentTypes,
                    allowsMultipleSelection: false,
                    legacyDocumentTypes: ["public.item", "public.data", "public.content"]
                ) { result in
                    showBIOSCompatibilityImporter = false
                    handleBIOSPickerResult(result, source: "compatibility")
                }
            }
        }
        .onAppear { loadBIOSes() }
    }

    private func biosRow(_ bios: ARMSX2BIOSInfo) -> some View {
        Button {
            ARMSX2Bridge.setDefaultBIOS(bios.fileName)
            defaultBIOS = bios.fileName
        } label: {
            HStack(spacing: 12) {
                regionBadge(for: bios)

                VStack(alignment: .leading, spacing: 4) {
                    Text(bios.fileName)
                        .font(.body)
                        .foregroundStyle(.primary)
                    Text(bios.valid ? "\(bios.regionName) BIOS" : settings.localized("Unknown BIOS Region"))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    if bios.valid && !bios.descriptionText.isEmpty {
                        Text(bios.descriptionText)
                            .font(.caption2.monospaced())
                            .foregroundStyle(.tertiary)
                            .lineLimit(1)
                    }
                }
                Spacer()
                if bios.fileName == defaultBIOS {
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
            Text(settings.localized("No BIOS Found"))
                .font(.title2)
                .fontWeight(.semibold)
            Text(settings.localized("Import a PS2 BIOS dump to enable booting."))
                .font(.body)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Button {
                NSLog("[ARMSX2 iOS BIOS] opening primary BIOS picker from empty state")
                showBIOSImporter = true
            } label: {
                Label(settings.localized("Import BIOS"), systemImage: "plus")
            }
            .buttonStyle(.borderedProminent)
            Button {
                NSLog("[ARMSX2 iOS BIOS] opening compatibility BIOS picker from empty state")
                showBIOSCompatibilityImporter = true
            } label: {
                Label(settings.localized("Compatibility Picker"), systemImage: "folder.badge.plus")
            }
            .buttonStyle(.bordered)
            Text(settings.localized("If one picker refuses to select your .bin/.rom file, try the other."))
                .font(.caption)
                .foregroundStyle(.tertiary)
                .multilineTextAlignment(.center)
        }
        .padding()
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func handleBIOSPickerResult(_ result: Result<[URL], Error>, source: String) {
        switch result {
        case .success(let urls):
            NSLog("[ARMSX2 iOS BIOS] %@ picker completed with %d URL(s)", source, urls.count)
            fileImporter.handleURLs(urls, preferredDestination: .bios)
            loadBIOSes()
            if defaultBIOS.isEmpty, let firstBIOS = bioses.first?.fileName {
                ARMSX2Bridge.setDefaultBIOS(firstBIOS)
                defaultBIOS = firstBIOS
            }
            if bioses.isEmpty, !urls.isEmpty {
                fileImporter.lastImportMessage = [
                    fileImporter.lastImportMessage,
                    "No usable PS2 BIOS was found after import. Use a 1-50 MB .bin or .rom BIOS dump."
                ]
                .compactMap { $0 }
                .joined(separator: "\n")
                fileImporter.showImportAlert = true
            }
        case .failure(let error):
            if (error as NSError).code != NSUserCancelledError {
                fileImporter.lastImportMessage = "BIOS import failed: \(error.localizedDescription)"
                fileImporter.showImportAlert = true
            }
        }
    }

    private func loadBIOSes() {
        bioses = ARMSX2Bridge.availableBIOSInfos()
        defaultBIOS = ARMSX2Bridge.defaultBIOSName()
    }

    private func regionBadge(for bios: ARMSX2BIOSInfo) -> some View {
        ZStack {
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(Color(.secondarySystemGroupedBackground))
                .frame(width: 44, height: 44)

            if let flag = flagEmoji(for: bios.countryCode) {
                Text(flag)
                    .font(.title2)
            } else {
                Image(systemName: "globe")
                    .font(.title3)
                    .foregroundStyle(.secondary)
            }
        }
        .accessibilityLabel(bios.valid ? "\(bios.regionName) BIOS" : "Unknown BIOS region")
    }

    private func flagEmoji(for countryCode: String) -> String? {
        let scalars = countryCode.uppercased().unicodeScalars
        guard scalars.count == 2 else { return nil }

        var unicodeScalars = String.UnicodeScalarView()
        for scalar in scalars {
            guard scalar.value >= 65, scalar.value <= 90,
                  let regional = UnicodeScalar(0x1F1E6 + scalar.value - 65) else {
                return nil
            }
            unicodeScalars.append(regional)
        }

        return String(unicodeScalars)
    }
}
