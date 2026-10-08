// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

/// Husk's visual vocabulary.
///
/// The app is drawn with the system's own colours and materials, so it is light or dark as the phone is (or as the user
/// pins it), with one accent the user chooses (`AppTheme`). Every screen says what it means -- a surface, dim text, the
/// accent -- through the names here rather than through colours of its own, which is what keeps the screens consistent.
///
/// The one thing that is not the system's is the chrome drawn over a running game (`huskPanel`, `OverlayButton`): always
/// dark, because it floats over a game's picture.
enum Theme {
    /// The page behind grouped content.
    static let bgUI = UIColor.systemGroupedBackground
    static let bg = Color(uiColor: bgUI)
    /// The page behind a screen of artwork: the library, a game's page.
    static let canvas = Color(uiColor: .systemBackground)
    /// Cards, rows, anything holding content.
    static let surface = Color(uiColor: .secondarySystemGroupedBackground)
    /// One step further up: wells, chips, the things that sit on a card.
    static let surfaceHigh = Color(uiColor: .tertiarySystemFill)
    /// The edge that separates a surface from the page.
    static let hairlineUI = UIColor.separator
    static let hairline = Color(uiColor: hairlineUI)

    static let textUI = UIColor.label
    static let text = Color(uiColor: textUI)
    static let textDim = Color(uiColor: .secondaryLabel)

    /// The user's accent: what is pressed, selected or switched on.
    static var accent: Color { AppTheme.shared.accentColor }
    static var accentSoft: Color { AppTheme.shared.accentColor.opacity(0.14) }
    static let good = Color(uiColor: .systemGreen)
    static let warn = Color(uiColor: .systemOrange)
    static let bad = Color(uiColor: .systemRed)
    /// What a floating thing casts.
    static let shadow = Color.black.opacity(0.18)

    /// Corner radii, from the largest surface to the smallest control.
    static let heroCorner: CGFloat = 26
    static let cardCorner: CGFloat = 20
    static let tileCorner: CGFloat = 18
    static let rowCorner: CGFloat = 14
    static let controlCorner: CGFloat = 14

    /// The margin every screen's content keeps from the edges.
    static let margin: CGFloat = 20

    /// The appearance the app is drawn in. System is the default: it follows the phone, light by day and
    /// dark by night. Light and Dark pin one.
    enum Appearance: String, CaseIterable, Identifiable {
        case system, light, dark

        static let key = "husk.appearance"

        var id: String { rawValue }

        var title: String {
            switch self {
            case .system: return "System"
            case .light: return "Light"
            case .dark: return "Dark"
            }
        }

        var style: UIUserInterfaceStyle {
            switch self {
            case .system: return .unspecified
            case .light: return .light
            case .dark: return .dark
            }
        }

        static var current: Appearance {
            Appearance(rawValue: UserDefaults.standard.string(forKey: key) ?? "") ?? .system
        }
    }

    /// Applies an appearance to every window the app has.
    ///
    /// Through UIKit rather than `.preferredColorScheme`: going back to "follow the system" means handing
    /// the window `nil`, and SwiftUI does not reliably let go of a scheme it has once forced. The window's
    /// own override does, and sheets and covers presented from it inherit it.
    static func apply(_ appearance: Appearance) {
        for case let scene as UIWindowScene in UIApplication.shared.connectedScenes {
            for window in scene.windows {
                window.overrideUserInterfaceStyle = appearance.style
            }
        }
    }

    static var backdrop: some View { bg.ignoresSafeArea() }
}

extension View {
    /// A container for content that is not a list row: a surface on the page, as the system draws one.
    @ViewBuilder
    func huskCard<S: Shape>(_ shape: S, high: Bool = false) -> some View {
        self.background(high ? Theme.surfaceHigh : Theme.surface, in: shape)
    }

    func huskCard(high: Bool = false) -> some View {
        huskCard(RoundedRectangle(cornerRadius: Theme.cardCorner, style: .continuous), high: high)
    }

    /// Chrome that sits over a running game: a dark, blurred panel with a hairline edge.
    ///
    /// Always dark, whatever the app's appearance, because it floats over a game's picture and the controls on it are
    /// drawn in white. The blur keeps it readable over a bright scene without hiding the game behind a solid block.
    func huskPanel<S: Shape>(_ shape: S) -> some View {
        self.background(.ultraThinMaterial, in: shape)
            .background(Color.black.opacity(0.55), in: shape)
            .overlay(shape.stroke(Color.white.opacity(0.12), lineWidth: 0.5))
            .environment(\.colorScheme, .dark)
    }
}

/// Technical values — sizes, counts, frame rates, commit hashes — are set in a
/// monospaced face so digits line up between rows and do not reflow as they
/// change.
extension Font {
    static func technical(_ size: CGFloat = 13, weight: Font.Weight = .regular) -> Font {
        .system(size: size, weight: weight, design: .monospaced)
    }

    /// The face for a screen's own title and for a game's name over its artwork.
    static func display(_ size: CGFloat, weight: Font.Weight = .bold) -> Font {
        .system(size: size, weight: weight, design: .rounded)
    }
}

/// The one action a screen is for.
struct PrimaryButtonStyle: ButtonStyle {
    var enabled = true

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 17, weight: .semibold, design: .rounded))
            .foregroundStyle(enabled ? .white : Theme.textDim)
            .frame(maxWidth: .infinity)
            .frame(height: 52)
            .background(enabled ? Theme.accent : Theme.surfaceHigh,
                        in: RoundedRectangle(cornerRadius: Theme.controlCorner, style: .continuous))
            .opacity(configuration.isPressed ? 0.85 : 1)
            .scaleEffect(configuration.isPressed ? 0.98 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}

/// The second action beside the primary one: the accent, softly.
struct SecondaryButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 17, weight: .semibold, design: .rounded))
            .foregroundStyle(Theme.accent)
            .frame(maxWidth: .infinity)
            .frame(height: 52)
            .background(Theme.accentSoft, in: RoundedRectangle(cornerRadius: Theme.controlCorner, style: .continuous))
            .opacity(configuration.isPressed ? 0.8 : 1)
            .scaleEffect(configuration.isPressed ? 0.98 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}

/// A card that is also a button: it gives a little under the finger.
struct CardButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.97 : 1)
            .opacity(configuration.isPressed ? 0.9 : 1)
            .animation(.easeOut(duration: 0.14), value: configuration.isPressed)
    }
}

/// Husk's mark, as drawn by whichever app icon is in use.
struct HuskMark: View {
    var size: CGFloat = 32
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        Group {
            if let art = HuskAppIcon.current.preview(dark: scheme == .dark) {
                Image(uiImage: art).resizable().scaledToFit()
            } else {
                Image(systemName: "cube.fill").font(.system(size: size * 0.6))
                    .foregroundStyle(Theme.accent)
            }
        }
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: size * 0.225, style: .continuous))
    }
}

/// The coloured square a settings row carries, as the Settings app draws them.
struct SettingsIcon: View {
    let systemImage: String
    let tint: Color

    var body: some View {
        RoundedRectangle(cornerRadius: 7, style: .continuous)
            .fill(tint.gradient)
            .frame(width: 29, height: 29)
            .overlay {
                Image(systemName: systemImage)
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(.white)
            }
    }
}

/// A small tag under a title — an engine, an ABI, a state.
struct Tag: View {
    let text: String
    var tint: Color = Theme.textDim
    var systemImage: String? = nil

    var body: some View {
        HStack(spacing: 4) {
            if let systemImage { Image(systemName: systemImage).font(.system(size: 10, weight: .bold)) }
            Text(text)
        }
        .font(.system(size: 12, weight: .semibold))
        .foregroundStyle(tint)
        .padding(.horizontal, 9).padding(.vertical, 4)
        .background(tint == Theme.textDim ? Theme.surfaceHigh : tint.opacity(0.14), in: Capsule())
    }
}

/// A label and a value on one line, for anything worth reading off.
struct DetailRow: View {
    let label: String
    let value: String
    var mono: Bool = true

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label).foregroundStyle(Theme.textDim)
            Spacer(minLength: 16)
            Text(value)
                .font(mono ? .technical() : .system(size: 15))
                .foregroundStyle(Theme.text)
                .multilineTextAlignment(.trailing)
                .textSelection(.enabled)
        }
        .font(.system(size: 15))
    }
}

/// Rows stacked into one card, hairlines between them.
struct RowGroup<Content: View>: View {
    @ViewBuilder var content: Content

    var body: some View {
        VStack(spacing: 0) { content }
            .huskCard()
    }
}

/// The hairline between two rows in a group.
struct RowDivider: View {
    var inset: CGFloat = 62

    var body: some View {
        Rectangle().fill(Theme.hairline)
            .frame(height: 0.5)
            .padding(.leading, inset)
    }
}

/// What a screen shows when it has nothing to show.
struct EmptyState: View {
    let title: String
    let message: String
    let systemImage: String
    var actionTitle: String? = nil
    var action: (() -> Void)? = nil

    var body: some View {
        VStack(spacing: 14) {
            Image(systemName: systemImage)
                .font(.system(size: 28, weight: .medium))
                .foregroundStyle(Theme.accent)
                .frame(width: 64, height: 64)
                .background(Theme.accentSoft, in: Circle())
            Text(title)
                .font(.display(20))
                .foregroundStyle(Theme.text)
            Text(message)
                .font(.system(size: 15))
                .foregroundStyle(Theme.textDim)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 28)
            if let actionTitle, let action {
                Button(actionTitle, action: action)
                    .buttonStyle(PrimaryButtonStyle())
                    .padding(.horizontal, 44)
                    .padding(.top, 6)
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 40)
    }
}

/// Work under way, said in one line.
struct BusyStrip: View {
    let text: String

    var body: some View {
        HStack(spacing: 12) {
            ProgressView()
            Text(text).font(.subheadline).lineLimit(2)
            Spacer(minLength: 0)
        }
        .padding(14)
        .huskCard(RoundedRectangle(cornerRadius: Theme.rowCorner, style: .continuous))
    }
}

/// One round control over a running game.
struct OverlayButton: View {
    let systemImage: String
    var label: String
    var active = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(active ? Theme.accent : .white)
                .frame(width: 38, height: 38)
                .background(active ? Color.white.opacity(0.16) : Color.white.opacity(0.08), in: Circle())
                .contentShape(Circle())
        }
        .buttonStyle(CardButtonStyle())
        .accessibilityLabel(label)
    }
}
