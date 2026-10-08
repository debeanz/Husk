// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UniformTypeIdentifiers

/// APKs handed to Husk: picked with the + button, shared from another app ("Husk" or "Copy to Husk" in the share sheet),
/// opened in Husk from Files, or downloaded from the Store.
///
/// Each arrives as a URL that may only be readable for a moment (a security-scoped file in another app's container, or a copy
/// in Documents/Inbox that iOS made), so it is copied straight away to a folder of Husk's own, then added to the library as a
/// game. Several shared at once -- a base APK and its splits -- arrive one URL at a time and are gathered into one game.
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

    static let extensions: Set<String> = ["apk", "xapk", "apkm", "apks"]

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

    /// Files picked inside Husk, or downloaded by the Store: all at once, so they are one game.
    func receive(_ urls: [URL]) {
        let wanted = urls.filter { Self.extensions.contains($0.pathExtension.lowercased()) }
        guard !wanted.isEmpty else { return }
        take(wanted) { self.deliver($0) }
    }

    /// Hand a set to the library, or queue it while the library is busy adding another.
    private func deliver(_ urls: [URL]) {
        guard !urls.isEmpty else { return }
        queue.append(urls)
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
