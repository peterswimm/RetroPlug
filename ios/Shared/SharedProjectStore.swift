import Foundation
import RetroPlugKit

// Canonical AU fullState persistence. The publisher supplies the App Group identifier in Info.plist
// under RetroPlugAppGroupIdentifier and adds the matching entitlement to both targets. Keeping that
// value out of source avoids claiming Tommy's bundle/signing namespace. Without it, persistence remains
// host-owned (AU fullState still works) and the standalone simply skips the shared-container mirror.
enum SharedProjectStore {
    private static var url: URL? {
        guard let group = Bundle.main.object(forInfoDictionaryKey: "RetroPlugAppGroupIdentifier") as? String,
              !group.isEmpty else { return nil }
        return FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: group)?
            .appendingPathComponent("current-project.plist")
    }

    static func restore(into unit: RetroPlugAudioUnit) {
        guard let url,
              let data = try? Data(contentsOf: url),
              let state = try? PropertyListSerialization.propertyList(from: data, format: nil)
                as? [String: Any] else { return }
        unit.fullState = state
    }

    static func save(from unit: RetroPlugAudioUnit) {
        guard let url, let state = unit.fullState,
              let data = try? PropertyListSerialization.data(fromPropertyList: state,
                                                              format: .binary, options: 0) else { return }
        try? data.write(to: url, options: .atomic)
    }
}
