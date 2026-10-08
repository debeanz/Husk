// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// A page pushed onto the Library: a game's page, its settings, or its technical details.
enum LibraryRoute: Hashable {
    case game(String)
    case gameSettings(String)
    case gameReport(String)
}

/// Where the tabs and the Library's stack are steered from.
///
/// One page can send you somewhere else -- a game's page opens its settings, a launch reopens the game Husk was closed to
/// switch to -- and a stack cannot be pushed from outside itself without somewhere to keep the path. This is that somewhere.
@MainActor final class Router: ObservableObject {
    static let shared = Router()

    @Published var tab: HuskTab = .library
    @Published var library: [LibraryRoute] = []

    /// A game to start as soon as its page is up: the one Husk was closed to switch to (see `switchTo`).
    @Published var autoPlay: String?

    private static let switchKey = "husk.switchToGame"

    /// Only one game can be loaded per run of Husk. Closing Husk to play another remembers which, and the next launch opens
    /// its page and starts it, so switching is: close, open, playing.
    func switchTo(_ appID: String) { UserDefaults.standard.set(appID, forKey: Self.switchKey) }

    /// At launch: the game Husk was closed to switch to, opened and started.
    func resumeSwitch() {
        guard let id = UserDefaults.standard.string(forKey: Self.switchKey) else { return }
        UserDefaults.standard.removeObject(forKey: Self.switchKey)
        HuskLog.log("ui", "opening \(id), which Husk was closed to switch to")
        tab = .library
        library = [.game(id)]
        autoPlay = id
    }

    /// Open a game's page from anywhere.
    func open(_ appID: String) {
        tab = .library
        library = [.game(appID)]
    }

    /// Pick APKs or bundles to add as games.
    func addGames() {
        HuskFilePicker.present { urls in IncomingFiles.shared.receive(urls) }
    }
}
