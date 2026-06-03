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

    private static let biosExtensionList = ["bin", "rom"]
    private static let gameExtensionList = ["iso", "chd", "img", "bin", "cue", "mdf", "cso", "zso", "gz", "elf"]
    private static let pnachExtensionList = ["pnach"]
    private static let biosExtensions = Set(biosExtensionList)
    private static let gameExtensions = Set(gameExtensionList)
    private static let pnachExtensions = Set(pnachExtensionList)
    // .bin files > 50MB are treated as game images, not BIOS
    private static let biosSizeThreshold: UInt64 = 50 * 1024 * 1024

    // BIOS dumps use loose/non-standard UTTypes on iOS. Keep the picker permissive
    // but include explicit .bin/.rom types so sideloaded Files providers expose them.
    static let biosContentTypes: [UTType] = broaderContentTypes(for: biosExtensionList)
    static let gameContentTypes: [UTType] = broaderContentTypes(for: gameExtensionList)
    static let pnachContentTypes: [UTType] = Array(Set([.item, .data, .content, .text, .plainText] + contentTypes(for: pnachExtensionList + ["txt", "patch"])))
    static let pnachImportNeedsGameMessage = "PNACH patches need to be imported for a specific game. Boot a game first or long-press a game in your library, then import the patch."

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
        NSLog("[ARMSX2 iOS Import] importing %d file(s), destination=%@", urls.count, String(describing: preferredDestination))

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
            case .unsupported(let message):
                rejected.append(message)
            case .failure(let message):
                failed.append(message)
            }
        }

        var lines: [String] = []
        if !imported.isEmpty {
            lines.append(imported.count == 1 ? imported[0] : "Imported \(imported.count) files.")
        }
        if !rejected.isEmpty {
            lines.append(rejected.joined(separator: "\n"))
        }
        if !failed.isEmpty {
            lines.append(failed.joined(separator: "\n"))
        }

        presentImportResult(lines.isEmpty ? "No files imported." : lines.joined(separator: "\n"))
        return importedGames
    }

    @discardableResult
    func importPNACHURLs(_ urls: [URL], asCheat: Bool = true, presentsAlert: Bool = true) -> String {
        let destinationPath = ARMSX2Bridge.pnachPathForCurrentGame(asCheat: asCheat)
        let results = urls.map { importPNACHFile($0, destinationPath: destinationPath, asCheat: asCheat) }
        return finishPNACHImport(results, presentsAlert: presentsAlert)
    }

    @discardableResult
    func importPNACHURLs(_ urls: [URL], forISO isoName: String, asCheat: Bool = true, presentsAlert: Bool = true) -> String {
        let destinationPath = ARMSX2Bridge.pnachPath(forISO: isoName, asCheat: asCheat)
        let results = urls.map { importPNACHFile($0, destinationPath: destinationPath, asCheat: asCheat) }
        return finishPNACHImport(results, presentsAlert: presentsAlert)
    }

    func presentImportResult(_ message: String) {
        lastImportMessage = message
        showImportAlert = true
    }

    private func finishPNACHImport(_ results: [ImportResult], presentsAlert: Bool) -> String {
        let messages = results.map { result -> String in
            switch result {
            case .success(let message, _):
                return message
            case .unsupported(let message):
                return message
            case .failure(let message):
                return message
            }
        }

        let message = messages.isEmpty ? "No PNACH files imported." : messages.joined(separator: "\n")
        if presentsAlert {
            presentImportResult(message)
        }
        return message
    }

    private enum ImportResult {
        case success(String, importedGame: ImportedGame? = nil)
        case unsupported(message: String)
        case failure(String)
    }

    private func importFile(_ url: URL, preferredDestination: ImportDestination) -> ImportResult {
        let accessing = url.startAccessingSecurityScopedResource()
        defer { if accessing { url.stopAccessingSecurityScopedResource() } }

        let ext = url.pathExtension.lowercased()
        let fileName = url.lastPathComponent
        NSLog("[ARMSX2 iOS Import] candidate: %@ ext=%@ securityScoped=%d",
              fileName, ext.isEmpty ? "(none)" : ext, accessing ? 1 : 0)

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
                return .unsupported(message: Self.unsupportedGameImportMessage(for: fileName))
            }
            destDir = (docsPath as NSString).appendingPathComponent("iso")
            category = "Game"
        } else if preferredDestination == .bios {
            guard Self.biosExtensions.contains(ext) else {
                NSLog("[ARMSX2 iOS Import] unsupported BIOS file: %@", fileName)
                return .unsupported(message: Self.unsupportedBIOSImportMessage(for: fileName))
            }

            let attrs = try? FileManager.default.attributesOfItem(atPath: url.path)
            let size = attrs?[.size] as? UInt64 ?? 0
            if size > 0 && size > Self.biosSizeThreshold {
                NSLog("[ARMSX2 iOS Import] rejecting oversized BIOS candidate: %@ size=%llu", fileName, size)
                return .failure(Self.oversizedBIOSImportMessage(for: fileName))
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
            return .unsupported(message: Self.unsupportedAutomaticImportMessage(for: fileName))
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
        let ext = url.pathExtension.lowercased()
        if !Self.pnachExtensions.contains(ext) {
            NSLog("[ARMSX2 iOS Import] PNACH file has non-standard extension, attempting text import: %@", fileName)
        }

        guard let destinationPath, !destinationPath.isEmpty else {
            return .failure(Self.pnachImportNeedsGameMessage)
        }

        let destinationURL = URL(fileURLWithPath: destinationPath)
        do {
            let data = try Data(contentsOf: url)
            guard let text = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .ascii) else {
                return .failure(Self.unreadablePNACHImportMessage(for: fileName))
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
            return .failure(Self.failedPNACHImportMessage(for: fileName, errorDescription: error.localizedDescription))
        }
    }

    static func isUserCancelledPickerError(_ error: Error) -> Bool {
        (error as NSError).code == NSUserCancelledError
    }

    static func failedGamePickerMessage(errorDescription: String) -> String {
        "Game import failed. Try importing a supported PS2 game image.\n\(errorDescription)"
    }

    static func failedBIOSPickerMessage(errorDescription: String) -> String {
        "BIOS import failed. Try importing a PS2 BIOS dump.\n\(errorDescription)"
    }

    static func failedPNACHPickerMessage(errorDescription: String) -> String {
        "PNACH import failed. Try importing a text patch for the selected game.\n\(errorDescription)"
    }

    private static func contentTypes(for extensions: [String]) -> [UTType] {
        extensions.map { ext in
            UTType(filenameExtension: ext) ?? UTType(importedAs: "com.armsx2.\(ext)", conformingTo: .data)
        }
    }

    private static func broaderContentTypes(for extensions: [String]) -> [UTType] {
        Array(Set([.item, .data, .content] + contentTypes(for: extensions)))
    }

    private static func unsupportedGameImportMessage(for fileName: String) -> String {
        "This does not look like a supported game image: \(fileName). Try importing a PS2 game file such as \(formatList(gameExtensionList))."
    }

    private static func unsupportedBIOSImportMessage(for fileName: String) -> String {
        "This does not look like a PS2 BIOS file: \(fileName). Try importing a BIOS dump such as \(formatList(biosExtensionList))."
    }

    private static func oversizedBIOSImportMessage(for fileName: String) -> String {
        "This file is too large to be a normal PS2 BIOS dump: \(fileName). Try importing a BIOS dump between 1 MB and 50 MB, usually \(formatList(biosExtensionList))."
    }

    private static func unsupportedAutomaticImportMessage(for fileName: String) -> String {
        "ARMSX2 could not import \(fileName). Try importing it from the matching Games, BIOS, or PNACH patch option."
    }

    private static func unreadablePNACHImportMessage(for fileName: String) -> String {
        "\(fileName) is not a readable PNACH patch. PNACH patches need to be UTF-8 or ASCII text."
    }

    private static func failedPNACHImportMessage(for fileName: String, errorDescription: String) -> String {
        "PNACH import failed for \(fileName). Check that this is a text patch for the selected game.\n\(errorDescription)"
    }

    private static func formatList(_ extensions: [String]) -> String {
        let formats = extensions.map { ".\($0)" }
        guard let last = formats.last else {
            return ""
        }
        if formats.count == 1 {
            return last
        }
        return "\(formats.dropLast().joined(separator: ", ")), or \(last)"
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

@MainActor
enum ARMSX2DeepLinkHandler {
    private static let supportedSchemes: Set<String> = ["armsx2", "armsx2-ios", "armsx2ios"]

    @discardableResult
    static func handle(_ url: URL) -> Bool {
        guard let scheme = url.scheme?.lowercased(), supportedSchemes.contains(scheme) else {
            return false
        }

        let route = routeComponents(for: url)
        if route.contains("library") || route.contains("export-library") || route.contains("games") {
            exportLibrary(from: url)
            return true
        }

        if route.contains("launch") || route.contains("boot") || route.contains("play") {
            launchGame(from: url)
            return true
        }

        showMessage("Unsupported ARMSX2 link: \(url.absoluteString)")
        NSLog("[ARMSX2 iOS DeepLink] unsupported url=%@", url.absoluteString)
        return true
    }

    private static func routeComponents(for url: URL) -> Set<String> {
        var components: [String] = []
        if let host = url.host, !host.isEmpty {
            components.append(host)
        }
        components.append(contentsOf: url.pathComponents.filter { $0 != "/" })
        return Set(components.map { $0.lowercased() })
    }

    private static func queryValue(_ names: [String], in url: URL) -> String? {
        if let components = URLComponents(url: url, resolvingAgainstBaseURL: false) {
            for name in names {
                if let value = components.queryItems?.first(where: { $0.name.caseInsensitiveCompare(name) == .orderedSame })?.value,
                   !value.isEmpty {
                    return value
                }
            }
        }

        // Some frontends send callback URLs without percent-encoding (for example,
        // callback=ludihub://armsx2-callback). Parse the raw query as a fallback.
        let wantedNames = Set(names.map { $0.lowercased() })
        guard let query = url.query else {
            return nil
        }

        for item in query.split(separator: "&", omittingEmptySubsequences: false) {
            let parts = item.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
            guard parts.count == 2 else {
                continue
            }

            let key = String(parts[0]).removingPercentEncoding ?? String(parts[0])
            guard wantedNames.contains(key.lowercased()) else {
                continue
            }

            let rawValue = String(parts[1])
            let value = rawValue.removingPercentEncoding ?? rawValue
            if !value.isEmpty {
                return value
            }
        }

        return nil
    }

    private static func exportLibrary(from url: URL) {
        guard let callback = queryValue(["callback", "callback_url", "return", "return_url", "x-success"], in: url) else {
            showMessage("LudiHub library export requested, but no callback URL was provided.")
            NSLog("[ARMSX2 iOS DeepLink] library export missing callback url=%@", url.absoluteString)
            return
        }

        let payload = libraryPayload()
        do {
            let data = try JSONSerialization.data(withJSONObject: payload, options: [.sortedKeys])
            let encoded = base64URLEncoded(data)
            guard let callbackURL = callbackURL(base: callback, payload: encoded) else {
                showMessage("LudiHub callback URL was invalid.")
                NSLog("[ARMSX2 iOS DeepLink] invalid callback=%@", callback)
                return
            }

            NSLog("[ARMSX2 iOS DeepLink] exporting library count=%d callback=%@", payload.gameCount, callbackURL.absoluteString)
            UIApplication.shared.open(callbackURL, options: [:]) { didOpen in
                NSLog("[ARMSX2 iOS DeepLink] callback open result=%d callback=%@", didOpen ? 1 : 0, callbackURL.absoluteString)
                if !didOpen {
                    Task { @MainActor in
                        showMessage("LudiHub callback could not be opened.")
                    }
                }
            }
        } catch {
            showMessage("LudiHub library export failed: \(error.localizedDescription)")
            NSLog("[ARMSX2 iOS DeepLink] library export failed: %@", error.localizedDescription)
        }
    }

    private static func libraryPayload() -> [String: Any] {
        let isoDir = URL(fileURLWithPath: ARMSX2Bridge.isoDirectory())
        let docsDir = URL(fileURLWithPath: ARMSX2Bridge.documentsDirectory())
        let gameNames = ARMSX2Bridge.availableISOs()
            .filter { !$0.lowercased().hasSuffix(".elf") }
            .sorted { $0.localizedCaseInsensitiveCompare($1) == .orderedAscending }

        let games: [[String: Any]] = gameNames.map { isoName in
            let metadata = ARMSX2Bridge.gameMetadata(forISO: isoName)
            let title = metadata["title"] ?? metadata["fileTitle"] ?? URL(fileURLWithPath: isoName).deletingPathExtension().lastPathComponent
            let isoURL = resolvedISOURL(isoName: isoName, isoDir: isoDir, docsDir: docsDir)
            let fileSize = fileSize(at: isoURL)
            let launchURL = "armsx2://launch?game=\(percentEncoded(isoName))"

            return [
                "title": title,
                "fileName": isoName,
                "serial": metadata["serial"] ?? "",
                "region": metadata["region"] ?? "",
                "crc": metadata["crc"] ?? "",
                "fileSize": fileSize,
                "fileType": isoURL.pathExtension.uppercased(),
                "launchURL": launchURL
            ]
        }

        return [
            "schema": "com.armsx2.library.v1",
            "app": "ARMSX2 iOS",
            "version": ARMSX2Bridge.buildVersion(),
            "generatedAt": ISO8601DateFormatter().string(from: Date()),
            "gameCount": games.count,
            "games": games
        ]
    }

    private static func resolvedISOURL(isoName: String, isoDir: URL, docsDir: URL) -> URL {
        let isoURL = isoDir.appendingPathComponent(isoName)
        if FileManager.default.fileExists(atPath: isoURL.path) {
            return isoURL
        }
        return docsDir.appendingPathComponent(isoName)
    }

    private static func fileSize(at url: URL) -> Int64 {
        guard let value = try? FileManager.default.attributesOfItem(atPath: url.path)[.size] as? NSNumber else {
            return 0
        }
        return value.int64Value
    }

    private static func launchGame(from url: URL) {
        guard let game = queryValue(["game", "iso", "file", "name"], in: url)?.removingPercentEncoding else {
            showMessage("ARMSX2 launch link is missing a game filename.")
            NSLog("[ARMSX2 iOS DeepLink] launch missing game url=%@", url.absoluteString)
            return
        }

        let available = Set(ARMSX2Bridge.availableISOs())
        guard available.contains(game) else {
            showMessage("ARMSX2 could not find \(game).")
            NSLog("[ARMSX2 iOS DeepLink] launch missing local game=%@", game)
            return
        }

        NSLog("[ARMSX2 iOS DeepLink] launching game=%@", game)
        AppState.shared.bootGame(isoName: game)
    }

    private static func callbackURL(base: String, payload: String) -> URL? {
        let decodedBase = base.removingPercentEncoding ?? base
        let candidates = decodedBase == base ? [base] : [base, decodedBase]

        for candidate in candidates {
            guard var components = URLComponents(string: candidate) else {
                continue
            }

            var items = components.queryItems ?? []
            items.append(URLQueryItem(name: "source", value: "armsx2-ios"))
            items.append(URLQueryItem(name: "payload", value: payload))
            components.queryItems = items
            if let url = components.url {
                return url
            }
        }

        return nil
    }

    private static func base64URLEncoded(_ data: Data) -> String {
        data.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }

    private static func percentEncoded(_ value: String) -> String {
        value.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) ?? value
    }

    private static func showMessage(_ message: String) {
        FileImportHandler.shared.presentImportResult(message)
    }
}

private extension Dictionary where Key == String, Value == Any {
    var gameCount: Int {
        self["gameCount"] as? Int ?? 0
    }
}

struct ImportDocumentPicker: UIViewControllerRepresentable {
    let allowedContentTypes: [UTType]
    let allowsMultipleSelection: Bool
    let legacyDocumentTypes: [String]?
    let onComplete: (Result<[URL], Error>) -> Void

    init(
        allowedContentTypes: [UTType],
        allowsMultipleSelection: Bool,
        legacyDocumentTypes: [String]? = nil,
        onComplete: @escaping (Result<[URL], Error>) -> Void
    ) {
        self.allowedContentTypes = allowedContentTypes
        self.allowsMultipleSelection = allowsMultipleSelection
        self.legacyDocumentTypes = legacyDocumentTypes
        self.onComplete = onComplete
    }

    func makeCoordinator() -> Coordinator {
        Coordinator(onComplete: onComplete)
    }

    func makeUIViewController(context: Context) -> UIDocumentPickerViewController {
        let picker: UIDocumentPickerViewController
        if let legacyDocumentTypes {
            picker = UIDocumentPickerViewController(documentTypes: legacyDocumentTypes, in: UIDocumentPickerMode.import)
            NSLog("[ARMSX2 iOS Import] opening legacy document picker types=%@ multiple=%d",
                  legacyDocumentTypes.joined(separator: ","), allowsMultipleSelection ? 1 : 0)
        } else {
            picker = UIDocumentPickerViewController(forOpeningContentTypes: allowedContentTypes, asCopy: true)
            NSLog("[ARMSX2 iOS Import] opening document picker types=%@ multiple=%d",
                  allowedContentTypes.map(\.identifier).joined(separator: ","), allowsMultipleSelection ? 1 : 0)
        }
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
            NSLog("[ARMSX2 iOS Import] picker returned %d URL(s): %@",
                  urls.count, urls.map(\.lastPathComponent).joined(separator: ", "))
            onComplete(.success(urls))
        }

        func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
            NSLog("[ARMSX2 iOS Import] picker cancelled")
            onComplete(.failure(CocoaError(.userCancelled)))
        }
    }
    
}

@objc class DeepLinkBridge: NSObject {
    @objc @discardableResult
    static func handle(_ url: URL) -> Bool {
        Task { @MainActor in
            _ = ARMSX2DeepLinkHandler.handle(url)
        }
        return true
    }
}
