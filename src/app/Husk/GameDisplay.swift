// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

/// How a game's picture fills the screen when the two are different shapes.
enum GameScaling: String, CaseIterable, Identifiable {
    /// All of the picture, with black bars around it.
    case fit
    /// The whole screen, with the picture's edges cut off.
    case fill
    /// The whole screen and all of the picture, out of shape.
    case stretch

    var id: String { rawValue }

    var title: String {
        switch self {
        case .fit: return "Fit"
        case .fill: return "Fill"
        case .stretch: return "Stretch"
        }
    }

    var detail: String {
        switch self {
        case .fit: return "Fit shows all of the game, with black bars where the shapes differ."
        case .fill: return "Fill covers the screen and cuts off the game's edges."
        case .stretch: return "Stretch covers the screen with all of the game, out of shape."
        }
    }
}

/// The size a game draws at and how it is put on screen: the defaults in Settings, and each game's own choice
/// (TLAppSettings), which is "default" to follow Settings.
///
/// The fixed sizes are Pearly's (debeanz/Pearly, GameSettings.resolutionOptions), so the two apps offer the same list.
enum GameDisplay {
    /// A game setting that follows Settings.
    static let followDefault = "default"
    /// Two pixels for each point of the screen: sharp, and a third of the pixels a 3x phone has. What Husk always did.
    static let automatic = "auto"
    /// Every pixel the screen has.
    static let screen = "screen"
    /// Fixed sizes, landscape. A portrait game gets them turned the other way.
    static let fixed = ["800x600", "1024x768", "1280x720", "1600x900", "1920x1080", "2048x1084", "2796x1290"]

    static let scalingKey = "husk.game.scaling"
    static let resolutionKey = "husk.game.resolution"

    /// Every resolution Settings offers.
    static var resolutions: [String] { [automatic, screen] + fixed }

    static func title(_ resolution: String) -> String {
        switch resolution {
        case followDefault: return "Default"
        case automatic: return "Automatic"
        case screen: return "Screen"
        default:
            guard let s = size(resolution) else { return resolution }
            return "\(Int(s.width)) × \(Int(s.height))"
        }
    }

    static func detail(_ resolution: String) -> String {
        switch resolution {
        case automatic: return "Automatic draws two pixels for each point of the screen: sharp, and light enough for most games."
        case screen: return "Screen draws every pixel the screen has: the sharpest, and the heaviest."
        default: return "\(title(resolution)) draws the game at that size, turned to the game's way up, and scales it to the screen."
        }
    }

    /// A fixed size's width and height, landscape; nil for Automatic and Screen.
    static func size(_ resolution: String) -> CGSize? {
        let parts = resolution.split(separator: "x").compactMap { Int($0) }
        guard parts.count == 2, parts[0] > 0, parts[1] > 0 else { return nil }
        return CGSize(width: max(parts[0], parts[1]), height: min(parts[0], parts[1]))
    }

    /// The Settings defaults.
    static var savedScaling: GameScaling {
        GameScaling(rawValue: UserDefaults.standard.string(forKey: scalingKey) ?? "") ?? .fit
    }
    static var savedResolution: String {
        let r = UserDefaults.standard.string(forKey: resolutionKey) ?? automatic
        return resolutions.contains(r) ? r : automatic
    }

    /// What a game runs with: its own choice, or Settings'.
    static func scaling(for s: TLAppSettings) -> GameScaling {
        GameScaling(rawValue: s.scaling) ?? savedScaling
    }
    static func resolution(for s: TLAppSettings) -> String {
        resolutions.contains(s.resolution) ? s.resolution : savedResolution
    }

    /// Pixels per point of the view before the game starts, and for Automatic and Screen the size it is told.
    @MainActor static func pointScale(for resolution: String) -> CGFloat {
        resolution == screen ? UIScreen.main.scale : 2
    }

    // MARK: frame rate

    /// The frame rates a game can be held to: the screen's refresh as the game is told it, and the most frames a second it
    /// shows. 60 is what Husk always did; 120 is a ProMotion screen's full rate.
    static let frameRateKey = "husk.game.framerate"
    static let frameRates = ["60", "120"]

    static func frameRateTitle(_ rate: String) -> String {
        rate == followDefault ? "Default" : "\(rate) Hz"
    }

    static func frameRateDetail(_ rate: String) -> String {
        rate == "120"
            ? "120 Hz lets a game run at up to 120 frames a second, where its own settings allow: smoother, and more battery "
              + "and heat. On a screen without ProMotion it is 60."
            : "60 Hz holds a game to 60 frames a second: what most Android games are made for, and easier on the battery."
    }

    /// The Settings default.
    static var savedFrameRate: String {
        let r = UserDefaults.standard.string(forKey: frameRateKey) ?? "60"
        return frameRates.contains(r) ? r : "60"
    }

    /// What a game runs with: its own choice, or Settings'.
    static func frameRate(for s: TLAppSettings) -> String {
        frameRates.contains(s.frameRate) ? s.frameRate : savedFrameRate
    }

    /// The rate a game is held to, in Hz: its choice, but no more than this screen shows.
    @MainActor static func frameRateHz(for s: TLAppSettings) -> Int {
        min(Int(frameRate(for: s)) ?? 60, max(UIScreen.main.maximumFramesPerSecond, 60))
    }
}
