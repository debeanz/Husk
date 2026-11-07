// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// A game's OBB files, which are kept in one place: `obb` in the game's own folder. The runtime tells the game that is its OBB folder, and
/// Android's path for it leads there too (tl_obb_folder), so nothing is linked or copied anywhere else, and clearing the game's data
/// leaves them alone.
enum GameOBB {
    struct Failure: LocalizedError {
        let errorDescription: String?
        init(_ what: String) { errorDescription = what }
    }

    static func folder(_ id: String) -> URL {
        TranslationLayer.root.appendingPathComponent(id, isDirectory: true).appendingPathComponent("obb", isDirectory: true)
    }

    /// The OBBs the game has.
    static func files(_ id: String) -> [URL] {
        ((try? FileManager.default.contentsOfDirectory(at: folder(id), includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.pathExtension.lowercased() == "obb" }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    /// The OBB a game will look for, when its APK says it has one. A Unity game built to keep its data apart from the APK (Split Application
    /// Binary) has `assets/unity_obb_guid`, and looks for main.<versionCode>.<package>.obb. Other engines say nothing as plain as this.
    static func wanted(_ app: TLApp) -> String? {
        for apk in app.apks where TranslationLayerStore.entry(apk, "assets/unity_obb_guid", limit: 4096) != nil {
            guard let manifest = TranslationLayerStore.entry(apk, "AndroidManifest.xml", limit: 8 << 20),
                  let package = ApkMetadata.packageName(manifest),
                  let version = ApkMetadata.versionCode(manifest) else { return nil }
            return "main.\(version).\(package).obb"
        }
        return nil
    }

    /// The name of the OBB the game needs and does not have.
    static func missing(_ app: TLApp) -> String? {
        guard let name = wanted(app) else { return nil }
        let fm = FileManager.default
        if fm.fileExists(atPath: folder(app.id).appendingPathComponent(name).path) { return nil }
        // Where an older Husk kept it: it moves to the folder when the game next starts.
        if let package = app.packageName {
            let old = TranslationLayer.root.appendingPathComponent(app.id, isDirectory: true)
                .appendingPathComponent("unity-data/sdcard/Android/obb/\(package)/\(name)")
            if fm.fileExists(atPath: old.path) { return nil }
        }
        return name
    }

    /// A picked file into the folder, under the name the game looks for when that is known (it finds nothing under any other).
    /// Returns the name it has now.
    static func add(_ url: URL, to app: TLApp, wanted: String?) throws -> String {
        guard url.pathExtension.lowercased() == "obb" else {
            throw Failure("\(url.lastPathComponent) is not an .obb file. If it came in a .zip, take it out of that first.")
        }
        var name = url.lastPathComponent
        if let wanted {
            name = name.lowercased().hasPrefix("patch.") ? "patch." + String(wanted.dropFirst("main.".count)) : wanted
        }
        let fm = FileManager.default
        let dir = folder(app.id)
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        // Copied under a name the game does not look for, and put in place whole. Coordinated, so that a file that lives in
        // iCloud and is not on the phone yet is downloaded first.
        let part = dir.appendingPathComponent(".adding-\(UUID().uuidString)")
        defer { try? fm.removeItem(at: part) }
        var coordinationError: NSError?
        var copyError: Error?
        NSFileCoordinator().coordinate(readingItemAt: url, options: [], error: &coordinationError) { readable in
            do { try fm.copyItem(at: readable, to: part) } catch { copyError = error }
        }
        if let failure = coordinationError ?? copyError { throw failure }
        let dest = dir.appendingPathComponent(name)
        try? fm.removeItem(at: dest)
        try fm.moveItem(at: part, to: dest)
        HuskLog.log("tl", "added \(url.lastPathComponent) to \(app.label) as \(name)")
        return name
    }
}
