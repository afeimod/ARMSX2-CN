// FileImportHandler.swift — Handle file import from Open-In / drag & drop
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import Foundation
import UniformTypeIdentifiers
import UIKit

@Observable
final class FileImportHandler: @unchecked Sendable {
    static let shared = FileImportHandler()

    struct ImportedGame: Sendable {
        let name: String
        let fileURL: URL
    }

    enum ImportDestination {
        case automatic
        case bios
        case game
        case pnachCheat
    }

    var lastImportMessage: String?
    var showImportAlert = false

    private static let biosExtensions: Set<String> = ["bin", "rom"]
    private static let gameExtensions: Set<String> = ["iso", "chd", "img", "bin", "cue", "mdf", "cso", "zso", "gz", "elf"]
    private static let pnachExtensions: Set<String> = ["pnach"]
    // .bin files > 50MB are treated as game images, not BIOS
    private static let biosSizeThreshold: UInt64 = 50 * 1024 * 1024

    static let biosContentTypes: [UTType] = broaderContentTypes(for: ["bin", "rom"])
    static let gameContentTypes: [UTType] = broaderContentTypes(for: ["iso", "chd", "img", "bin", "cue", "mdf", "cso", "zso", "gz", "elf"])
    static let pnachContentTypes: [UTType] = broaderContentTypes(for: ["pnach"])

    private init() {}

    @discardableResult
    func handleURL(_ url: URL) -> [ImportedGame] {
        handleURLs([url], preferredDestination: .automatic)
    }

    @discardableResult
    func handleURLs(_ urls: [URL], preferredDestination: ImportDestination = .automatic) -> [ImportedGame] {
        importURLs(urls, preferredDestination: preferredDestination)
    }

    @discardableResult
    func importURLs(_ urls: [URL], preferredDestination: ImportDestination = .automatic) -> [ImportedGame] {
        var imported: [String] = []
        var importedGames: [ImportedGame] = []
        var rejected: [String] = []
        var failed: [String] = []

        for url in urls {
            switch importFile(url, preferredDestination: preferredDestination) {
            case .success(let message, let importedGame):
                imported.append(message)
                if let importedGame {
                    importedGames.append(importedGame)
                }
            case .unsupported(let fileName):
                rejected.append(fileName)
            case .failure(let message):
                failed.append(message)
            }
        }

        var lines: [String] = []
        if !imported.isEmpty {
            lines.append(imported.count == 1 ? imported[0] : "Imported \(imported.count) files.")
        }
        if !rejected.isEmpty {
            lines.append("Unsupported: \(rejected.joined(separator: ", "))")
        }
        if !failed.isEmpty {
            lines.append(failed.joined(separator: "\n"))
        }

        lastImportMessage = lines.isEmpty ? "No files imported." : lines.joined(separator: "\n")
        showImportAlert = true
        return importedGames
    }

    @discardableResult
    func importPNACHURLs(_ urls: [URL], asCheat: Bool = true) -> String {
        let destinationPath = ARMSX2Bridge.pnachPathForCurrentGame(asCheat: asCheat)
        let results = urls.map { importPNACHFile($0, destinationPath: destinationPath, asCheat: asCheat) }
        return finishPNACHImport(results)
    }

    @discardableResult
    func importPNACHURLs(_ urls: [URL], forISO isoName: String, asCheat: Bool = true) -> String {
        let destinationPath = ARMSX2Bridge.pnachPath(forISO: isoName, asCheat: asCheat)
        let results = urls.map { importPNACHFile($0, destinationPath: destinationPath, asCheat: asCheat) }
        return finishPNACHImport(results)
    }

    private func finishPNACHImport(_ results: [ImportResult]) -> String {
        let messages = results.map { result -> String in
            switch result {
            case .success(let message, _):
                return message
            case .unsupported(let fileName):
                return "Unsupported: \(fileName)"
            case .failure(let message):
                return message
            }
        }

        let message = messages.isEmpty ? "No PNACH files imported." : messages.joined(separator: "\n")
        lastImportMessage = message
        showImportAlert = true
        return message
    }

    private enum ImportResult {
        case success(String, importedGame: ImportedGame? = nil)
        case unsupported(String)
        case failure(String)
    }

    private func importFile(_ url: URL, preferredDestination: ImportDestination) -> ImportResult {
        let accessing = url.startAccessingSecurityScopedResource()
        defer { if accessing { url.stopAccessingSecurityScopedResource() } }

        let ext = url.pathExtension.lowercased()
        let fileName = url.lastPathComponent

        if preferredDestination == .pnachCheat || (preferredDestination == .automatic && Self.pnachExtensions.contains(ext)) {
            return importPNACHFile(url, destinationPath: ARMSX2Bridge.pnachPathForCurrentGame(asCheat: true), asCheat: true)
        }

        let docsPath = NSSearchPathForDirectoriesInDomains(.documentDirectory, .userDomainMask, true).first!

        // Determine destination
        let destDir: String
        let category: String

        if preferredDestination == .game {
            guard Self.gameExtensions.contains(ext) else {
                NSLog("[ARMSX2 iOS Import] unsupported game file: %@", fileName)
                return .unsupported(fileName)
            }
            destDir = (docsPath as NSString).appendingPathComponent("iso")
            category = "Game"
        } else if preferredDestination == .bios {
            guard Self.biosExtensions.contains(ext) else {
                NSLog("[ARMSX2 iOS Import] unsupported BIOS file: %@", fileName)
                return .unsupported(fileName)
            }
            destDir = (docsPath as NSString).appendingPathComponent("bios")
            category = "BIOS"
        } else if Self.gameExtensions.subtracting(Self.biosExtensions).contains(ext) {
            destDir = (docsPath as NSString).appendingPathComponent("iso")
            category = "Game"
        } else if Self.biosExtensions.contains(ext) {
            // Check file size to distinguish BIOS (.bin ~4MB) from game (.bin ~700MB)
            let attrs = try? FileManager.default.attributesOfItem(atPath: url.path)
            let size = attrs?[.size] as? UInt64 ?? 0
            if ext == "bin" && size > Self.biosSizeThreshold {
                destDir = (docsPath as NSString).appendingPathComponent("iso")
                category = "Game"
            } else {
                destDir = (docsPath as NSString).appendingPathComponent("bios")
                category = "BIOS"
            }
        } else {
            NSLog("[ARMSX2 iOS Import] unsupported file: %@", fileName)
            return .unsupported(fileName)
        }

        // Create directory if needed
        try? FileManager.default.createDirectory(atPath: destDir, withIntermediateDirectories: true)

        let destPath = (destDir as NSString).appendingPathComponent(fileName)

        // Copy file
        do {
            if FileManager.default.fileExists(atPath: destPath) {
                try FileManager.default.removeItem(atPath: destPath)
            }
            try copyImportedFile(from: url, to: URL(fileURLWithPath: destPath))
            NSLog("[ARMSX2 iOS Import] %@ imported: %@ -> %@", category, fileName, destPath)
            let importedGame = category == "Game" ? ImportedGame(name: fileName, fileURL: URL(fileURLWithPath: destPath)) : nil
            return .success("\(category) imported: \(fileName)", importedGame: importedGame)
        } catch {
            NSLog("[ARMSX2 iOS Import] failed: %@ -> %@ error=%@", fileName, destPath, error.localizedDescription)
            return .failure("\(fileName): \(error.localizedDescription)")
        }
    }

    private func importPNACHFile(_ url: URL, destinationPath: String?, asCheat: Bool) -> ImportResult {
        let accessing = url.startAccessingSecurityScopedResource()
        defer { if accessing { url.stopAccessingSecurityScopedResource() } }

        let fileName = url.lastPathComponent
        guard Self.pnachExtensions.contains(url.pathExtension.lowercased()) else {
            NSLog("[ARMSX2 iOS Import] unsupported PNACH file: %@", fileName)
            return .unsupported(fileName)
        }

        guard let destinationPath, !destinationPath.isEmpty else {
            return .failure("Boot the target game before importing \(fileName), so ARMSX2 can rename it to Serial_CRC.pnach.")
        }

        let destinationURL = URL(fileURLWithPath: destinationPath)
        do {
            let data = try Data(contentsOf: url)
            guard let text = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .ascii) else {
                return .failure("\(fileName): PNACH must be UTF-8 or ASCII text.")
            }

            let normalized = Self.normalizedPNACHText(text)
            try FileManager.default.createDirectory(at: destinationURL.deletingLastPathComponent(), withIntermediateDirectories: true)
            if FileManager.default.fileExists(atPath: destinationURL.path) {
                try FileManager.default.removeItem(at: destinationURL)
            }
            try normalized.write(to: destinationURL, atomically: true, encoding: .utf8)

            if asCheat {
                ARMSX2Bridge.setINIBool("EmuCore", key: "EnableCheats", value: true)
            }
            ARMSX2Bridge.reloadPatches()

            NSLog("[ARMSX2 iOS Import] PNACH imported: %@ -> %@", fileName, destinationURL.path)
            return .success("PNACH imported as \(destinationURL.lastPathComponent)")
        } catch {
            NSLog("[ARMSX2 iOS Import] PNACH failed: %@ -> %@ error=%@", fileName, destinationURL.path, error.localizedDescription)
            return .failure("\(fileName): \(error.localizedDescription)")
        }
    }

    private static func contentTypes(for extensions: [String]) -> [UTType] {
        extensions.map { ext in
            UTType(filenameExtension: ext) ?? UTType(importedAs: "com.armsx2.\(ext)", conformingTo: .data)
        }
    }

    private static func broaderContentTypes(for extensions: [String]) -> [UTType] {
        Array(Set([.item, .data, .content] + contentTypes(for: extensions)))
    }

    private func copyImportedFile(from sourceURL: URL, to destinationURL: URL) throws {
        do {
            try FileManager.default.copyItem(at: sourceURL, to: destinationURL)
        } catch {
            NSLog("[ARMSX2 iOS Import] copyItem failed for %@, retrying with Data: %@",
                  sourceURL.lastPathComponent, error.localizedDescription)
            let data = try Data(contentsOf: sourceURL)
            try data.write(to: destinationURL, options: .atomic)
        }
    }

    private static func normalizedPNACHText(_ text: String) -> String {
        var normalized = text
            .replacingOccurrences(of: "\r\n", with: "\n")
            .replacingOccurrences(of: "\r", with: "\n")

        if !normalized.hasSuffix("\n") {
            normalized.append("\n")
        }

        return normalized
    }
}

struct ImportDocumentPicker: UIViewControllerRepresentable {
    let allowedContentTypes: [UTType]
    let allowsMultipleSelection: Bool
    let onComplete: (Result<[URL], Error>) -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(onComplete: onComplete)
    }

    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: allowedContentTypes, asCopy: true)
        picker.allowsMultipleSelection = allowsMultipleSelection
        picker.delegate = context.coordinator
        return picker
    }

    func updateUIViewController(_ uiViewController: UIDocumentPickerViewController, context: Context) {}

    final class Coordinator: NSObject, UIDocumentPickerDelegate {
        private let onComplete: (Result<[URL], Error>) -> Void

        init(onComplete: @escaping (Result<[URL], Error>) -> Void) {
            self.onComplete = onComplete
        }

        func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
            onComplete(.success(urls))
        }

        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
            onComplete(.failure(CocoaError(.userCancelled)))
        }
    }
}
