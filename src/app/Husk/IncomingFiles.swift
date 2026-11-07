// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UniformTypeIdentifiers

/// APKs handed to Husk: picked with the + button, shared from another app ("Husk" or "Copy to Husk" in the share sheet),
/// opened in Husk from Files, or downloaded from the Store.
///
/// Each arrives as a URL that may only be readable for a moment (a security-scoped file in another app's container, or a copy
/// in Documents/Inbox that iOS made), so it is copied straight away to a folder of Husk's own, then added to the library. Several
/// shared at once arrive one URL at a time and are gathered; whatever comes together is then sorted into games by package name, so
/// that a base APK and its splits are one game and two games picked at the same time are two.
@MainActor
final class IncomingFiles: ObservableObject {
    static let shared = IncomingFiles()

    private var gathering: [URL] = []
    private var gatherTask: Task<Void, Never>?
    /// Sets of files waiting for the library to finish adding the one before.
    private var queue: [[URL]] = []
    private var draining = false

    /// Where copies wait. Emptied when Husk starts, so nothing left over takes up space.
    nonisolated static var folder: URL {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("Incoming", isDirectory: true)
    }

    nonisolated static func clearLeftovers() {
        try? FileManager.default.removeItem(at: folder)
    }

    static let extensions: Set<String> = ["apk", "xapk", "apkm", "apks", "obb"]

    /// Copies under way, said in one line while they last.
    @Published private(set) var preparing: String?

    /// A URL iOS opened Husk with. Anything that is not an APK or a bundle of them is left alone (husk:// links are not files).
    func receive(_ url: URL) {
        guard url.isFileURL, Self.extensions.contains(url.pathExtension.lowercased()) else { return }
        take([url]) { copies in
            self.gathering += copies
            // The pieces of a split set arrive one after another: add them once they have all come.
            self.gatherTask?.cancel()
            self.gatherTask = Task {
                try? await Task.sleep(nanoseconds: 600_000_000)
                guard !Task.isCancelled else { return }
                let set = self.gathering
                self.gathering = []
                self.deliver(set)
            }
        }
    }

    /// Files picked inside Husk, or downloaded by the Store: all at once.
    func receive(_ urls: [URL]) {
        let wanted = urls.filter { Self.extensions.contains($0.pathExtension.lowercased()) }
        guard !wanted.isEmpty else { return }
        take(wanted) { self.deliver($0) }
    }

    /// What came together, as games. A split has its base APK's package name, so APKs go by that. A bundle is a game already. An OBB's name says
    /// whose it is (main.123.com.example.game.obb); one that fits no APK here goes to the only game there is, or stands alone, and the library
    /// says that it needs its game. That one comes last, so that what the library says is what is left on the screen.
    /// An APK whose manifest cannot be read stays with the first APK, as there is nothing to tell it apart by.
    nonisolated static func games(in urls: [URL]) -> [[URL]] {
        var games: [[URL]] = []
        var gameOf: [String: Int] = [:]
        var unknown: [URL] = []
        var obbs: [URL] = []
        for url in urls {
            switch url.pathExtension.lowercased() {
            case "obb":
                obbs.append(url)
            case "apk":
                guard let manifest = TranslationLayerStore.entry(url.path, "AndroidManifest.xml", limit: 8 << 20),
                      let package = ApkMetadata.packageName(manifest) else {
                    unknown.append(url)
                    continue
                }
                if let game = gameOf[package] {
                    games[game].append(url)
                } else {
                    gameOf[package] = games.count
                    games.append([url])
                }
            default:
                games.append([url])
            }
        }
        if !unknown.isEmpty {
            if let first = gameOf.values.min() { games[first] += unknown } else { games.append(unknown) }
        }
        var homeless: [URL] = []
        for obb in obbs {
            let name = obb.deletingPathExtension().lastPathComponent.split(separator: ".", maxSplits: 2)
            if name.count == 3, let game = gameOf[String(name[2])] {
                games[game].append(obb)
            } else if games.count == 1 {
                games[0].append(obb)
            } else {
                homeless.append(obb)
            }
        }
        if !homeless.isEmpty { games.append(homeless) }
        return games
    }

    /// Sort a set into games, away from the main thread as a manifest is read out of each APK, and hand them to the library.
    private func deliver(_ urls: [URL]) {
        guard !urls.isEmpty else { return }
        Task.detached(priority: .userInitiated) {
            let games = Self.games(in: urls)
            if games.count > 1 { HuskLog.log("ui", "\(urls.count) files together are \(games.count) games") }
            await MainActor.run { self.enqueue(games) }
        }
    }

    /// Hand games to the library, or queue them while the library is busy adding another.
    private func enqueue(_ games: [[URL]]) {
        queue += games
        guard !draining else { return }
        draining = true
        Task {
            while !queue.isEmpty {
                let store = TranslationLayerStore.shared
                while store.busy != nil { try? await Task.sleep(nanoseconds: 300_000_000) }
                let next = queue.removeFirst()
                // Moved, not copied: the copy here is Husk's own, and a game can be gigabytes.
                store.add(next, move: true)
                // Let the store take it before looking at the next one.
                try? await Task.sleep(nanoseconds: 200_000_000)
            }
            draining = false
        }
    }

    /// Copy files to Husk's own folder, off the main thread -- a game can be gigabytes -- then hand back the copies.
    private func take(_ urls: [URL], then: @escaping @MainActor ([URL]) -> Void) {
        preparing = urls.count == 1 ? "Preparing \(urls[0].lastPathComponent)…" : "Preparing \(urls.count) files…"
        Task.detached(priority: .userInitiated) {
            var copies: [URL] = []
            for url in urls {
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                let dir = Self.folder.appendingPathComponent(UUID().uuidString, isDirectory: true)
                let copy = dir.appendingPathComponent(url.lastPathComponent)
                do {
                    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
                    try FileManager.default.copyItem(at: url, to: copy)
                    copies.append(copy)
                    // iOS's own copy (Documents/Inbox) is no use once Husk has one.
                    if url.path.contains("/Documents/Inbox/") { try? FileManager.default.removeItem(at: url) }
                    HuskLog.log("ui", "took in \(url.lastPathComponent)")
                } catch {
                    HuskLog.log("ui", "could not take in \(url.lastPathComponent): \(error.localizedDescription)")
                }
            }
            await MainActor.run {
                self.preparing = nil
                if !copies.isEmpty { then(copies) }
            }
        }
    }
}
