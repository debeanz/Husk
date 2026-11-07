// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// Takes what a game needs out of an app bundle: an .xapk (APKPure), .apkm (APKMirror) or .apks (bundletool), which are zip files holding a base APK and its splits and asset packs,
/// and in an .xapk, often its OBBs. The unpacking itself is husk_tl_bundle_unpack, in the runtime: it reads any zip the runtime can, ZIP64 and compressed entries included, a few MB at a time.
enum BundleUnpacker {
    static let extensions: Set<String> = ["xapk", "apkm", "apks"]

    enum Failure: LocalizedError {
        case unsupported(String)
        var errorDescription: String? {
            switch self {
            case .unsupported(let why): return why
            }
        }
    }

    /// Whether an APK is a split or an asset pack, as the bundle formats name them, rather than the app itself. Some .xapk files number theirs
    /// (split1.apk, split2.apk beside base.apk).
    static func isSplit(_ name: String) -> Bool {
        let n = name.lowercased()
        if n == "base.apk" || n == "base-master.apk" { return false }
        let numbered = n.hasPrefix("split") && n.dropFirst(5).first.map(\.isNumber) == true
        return n.hasPrefix("config.") || n.hasPrefix("split_") || numbered || n.hasPrefix("asset_pack") || n.hasPrefix("base-") || n.hasPrefix("install_time")
    }

    /// The order an app's APKs are given to the runtime in, which takes only four: the app itself, then what it cannot run without (its 64-bit libraries, its asset pack),
    /// then the language and screen-density splits, which it does not need.
    ///
    /// `siblings` are the app's other APK names. Besides Android's own split names (config.*, split_*), a split may be named
    /// after the base: com.game.apk beside com.game.config.arm64_v8a.apk and com.game.SomeAssetPack.apk, the way Google
    /// Play hands them out. A name that is another's name plus ".something" is that one's split.
    static func rank(_ name: String, siblings: [String] = []) -> Int {
        let n = name.lowercased()
        let named = siblings.contains { other in
            let o = other.lowercased()
            guard o != n, o.hasSuffix(".apk") else { return false }
            return n.hasPrefix(String(o.dropLast(4)) + ".")
        }
        if !isSplit(n) && !named { return 0 }
        if n.contains("arm64") { return 1 }
        if n.hasPrefix("asset_pack") || n.hasPrefix("install_time") || (named && !n.contains(".config.")) { return 2 }
        return 3
    }

    /// Copies every APK in `bundle` into `dir`, and any OBBs into `dir`/obb, where the runtime finds them at launch. An APK that only carries
    /// zipped asset packs is not copied: its packs are unpacked into `dir`/assetpacks instead. Returns the names of the APKs and OBBs kept.
    @discardableResult
    static func unpack(_ bundle: URL, into dir: URL) throws -> [String] {
        let scoped = bundle.startAccessingSecurityScopedResource()
        defer { if scoped { bundle.stopAccessingSecurityScopedResource() } }
        let room = 400
        var err = [CChar](repeating: 0, count: room)
        guard let raw = husk_tl_bundle_unpack(bundle.path, dir.path, &err, room) else {
            throw Failure.unsupported(String(cString: err))
        }
        defer { husk_tl_free(raw) }
        return String(cString: raw).split(separator: "\n").map(String.init)
    }
}
