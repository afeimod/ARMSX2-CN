// FileImportHandler.swift — Handle file import from Open-In / drag & drop
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import Foundation
import UniformTypeIdentifiers

@Observable
final class FileImportHandler: @unchecked Sendable {
    static let shared = FileImportHandler()

    enum ImportDestination {
        case automatic
        case bios
        case game
        case pnachCheat
    }

    var lastImportMessage: String?
    var showImportAlert = false

    private static let biosExtensions: Set<String> = ["bin", "rom"]
    private static let gameExtensions: Set<String> = ["iso", "chd", "img", "bin", "cso", "zso", "gz", "elf"]
    private static let pnachExtensions: Set<String> = ["pnach"]
    // .bin files > 50MB are treated as game images, not BIOS
    private static let biosSizeThreshold: UInt64 = 50 * 1024 * 1024

    static let biosContentTypes: [UTType] = contentTypes(for: ["bin", "rom"])
    static let gameContentTypes: [UTType] = contentTypes(for: ["iso", "chd", "img", "bin", "cso", "zso", "gz", "elf"])
    static let pnachContentTypes: [UTType] = contentTypes(for: ["pnach"])

    private init() {}

    func handleURL(_ url: URL) {
        handleURLs([url], preferredDestination: .automatic)
    }

    func handleURLs(_ urls: [URL], preferredDestination: ImportDestination = .automatic) {
        var imported: [String] = []
        var rejected: [String] = []
        var failed: [String] = []

        for url in urls {
            switch importFile(url, preferredDestination: preferredDestination) {
            case .success(let message):
                imported.append(message)
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
    }

    @discardableResult
    func importPNACHURLs(_ urls: [URL], asCheat: Bool = true) -> String {
        let results = urls.map { importPNACHFile($0, asCheat: asCheat) }
        let messages = results.map { result -> String in
            switch result {
            case .success(let message):
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
        case success(String)
        case unsupported(String)
        case failure(String)
    }

    private func importFile(_ url: URL, preferredDestination: ImportDestination) -> ImportResult {
        let accessing = url.startAccessingSecurityScopedResource()
        defer { if accessing { url.stopAccessingSecurityScopedResource() } }

        let ext = url.pathExtension.lowercased()
        let fileName = url.lastPathComponent

        if preferredDestination == .pnachCheat || (preferredDestination == .automatic && Self.pnachExtensions.contains(ext)) {
            return importPNACHFile(url, asCheat: true)
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
            try FileManager.default.copyItem(at: url, to: URL(fileURLWithPath: destPath))
            NSLog("[ARMSX2 iOS Import] %@ imported: %@ -> %@", category, fileName, destPath)
            return .success("\(category) imported: \(fileName)")
        } catch {
            NSLog("[ARMSX2 iOS Import] failed: %@ -> %@ error=%@", fileName, destPath, error.localizedDescription)
            return .failure("\(fileName): \(error.localizedDescription)")
        }
    }

    private func importPNACHFile(_ url: URL, asCheat: Bool) -> ImportResult {
        let accessing = url.startAccessingSecurityScopedResource()
        defer { if accessing { url.stopAccessingSecurityScopedResource() } }

        let fileName = url.lastPathComponent
        guard Self.pnachExtensions.contains(url.pathExtension.lowercased()) else {
            NSLog("[ARMSX2 iOS Import] unsupported PNACH file: %@", fileName)
            return .unsupported(fileName)
        }

        guard let destinationPath = ARMSX2Bridge.pnachPathForCurrentGame(asCheat: asCheat), !destinationPath.isEmpty else {
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
