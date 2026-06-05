// SPDX-License-Identifier: GPL-3.0+

import Foundation
import SwiftUI
import UniformTypeIdentifiers
import UIKit

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
