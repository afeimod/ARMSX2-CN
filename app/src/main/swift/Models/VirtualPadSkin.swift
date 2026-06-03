// VirtualPadSkin.swift - virtual pad skin model for SwiftUI
// SPDX-License-Identifier: GPL-3.0+

import Foundation

enum VirtualPadSkin: Int, CaseIterable, Identifiable {
    case armsx2Refresh = 0
    case crispVector = 1
    case custom = 2

    var id: Int { rawValue }

    var label: String {
        switch self {
        case .armsx2Refresh:
            return "ARMSX2 Refresh"
        case .crispVector:
            return "Crisp Vector"
        case .custom:
            return "Custom Imported"
        }
    }

    var detail: String {
        switch self {
        case .armsx2Refresh:
            return "Uses the bundled ARMSX2 refresh button art with stronger press feedback."
        case .crispVector:
            return "Draws the pad in SwiftUI for sharper outlines at any screen scale."
        case .custom:
            return "Loads user-imported button images or a full portrait/landscape skin from the custom skin folder."
        }
    }

    static func customSkinDirectory(create: Bool = false) -> URL? {
        guard let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else {
            return nil
        }

        let directory = documents.appendingPathComponent("ControllerSkins/Custom", isDirectory: true)
        if create {
            try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        }
        return directory
    }
}
