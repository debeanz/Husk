// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// Whether the welcome has been seen.
///
/// Versioned rather than a plain "seen it" flag: a later build that adds a page existing installs must see can raise
/// `version`, and only that reopens it.
enum Onboarding {
    /// Raise this when a page is added that existing installs must see.
    static let version = 1

    private static let key = "husk.onboardingVersion"

    static var needed: Bool {
        UserDefaults.standard.integer(forKey: key) < version
    }

    static func complete() {
        UserDefaults.standard.set(version, forKey: key)
    }
}

/// The first thing a new install shows: what Husk is, turning on JIT, and how games get in.
struct OnboardingView: View {
    let onDone: () -> Void

    @State private var page = 0
    @Environment(\.colorScheme) private var scheme
    @ObservedObject private var jit = JITCoordinator.shared
    @State private var settingUpJIT = false

    private let pages = 3

    var body: some View {
        ZStack {
            Theme.canvas.ignoresSafeArea()

            VStack(spacing: 0) {
                TabView(selection: $page) {
                    welcome.tag(0)
                    jitPage.tag(1)
                    ready.tag(2)
                }
                .tabViewStyle(.page(indexDisplayMode: .never))
                .sheet(isPresented: $settingUpJIT) { JITSetupFlow() }

                // One control, always in the same place. A flow that moves its own button around is harder to get through
                // than one that does not, and this is the first thing anyone sees.
                VStack(spacing: 16) {
                    HStack(spacing: 6) {
                        ForEach(0..<pages, id: \.self) { i in
                            Capsule()
                                .fill(i == page ? Theme.accent : Color.secondary.opacity(0.3))
                                .frame(width: i == page ? 20 : 7, height: 7)
                                .animation(.snappy, value: page)
                        }
                    }
                    Button {
                        if page < pages - 1 {
                            withAnimation(.snappy) { page += 1 }
                        } else {
                            Onboarding.complete()
                            HuskLog.log("ui", "welcome complete")
                            onDone()
                        }
                    } label: {
                        Text(page < pages - 1 ? "Continue" : "Get Started")
                    }
                    .buttonStyle(PrimaryButtonStyle())
                    .padding(.horizontal, 28)
                }
                .padding(.bottom, 28)
            }
        }
    }

    // MARK: pages

    private var welcome: some View {
        VStack(spacing: 18) {
            Spacer()
            if let art = HuskAppIcon.current.preview(dark: scheme == .dark) {
                Image(uiImage: art)
                    .resizable().scaledToFit()
                    .frame(width: 116, height: 116)
                    .clipShape(RoundedRectangle(cornerRadius: 26, style: .continuous))
                    .shadow(color: Theme.accent.opacity(0.35), radius: 24, y: 10)
            }
            Text("Husk").font(.display(42, weight: .heavy))
            Text("Android games, running natively on your iPhone.")
                .font(.title3).foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 30)
            VStack(alignment: .leading, spacing: 16) {
                feature("bolt.fill", "No Android to boot",
                        "A game's own code runs straight on the iPhone's processor, so it starts in seconds.")
                feature("gamecontroller.fill", "Made for games",
                        "Unity, Unreal, cocos2d-x, Godot, SDL and more — with touch, sound and controllers.")
                feature("hand.tap.fill", "Nothing in the way",
                        "Games fill the screen. Swipe down from the top edge for controls when you need them.")
            }
            .padding(.horizontal, 32)
            .padding(.top, 12)
            Spacer()
        }
    }

    private func feature(_ icon: String, _ title: String, _ detail: String) -> some View {
        HStack(alignment: .top, spacing: 14) {
            Image(systemName: icon)
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(Theme.accent)
                .frame(width: 36, height: 36)
                .background(Theme.accentSoft, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.subheadline.weight(.semibold))
                Text(detail).font(.subheadline).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    /// What the JIT page says is already in place, if anything.
    private var jitState: String? {
        if JITBootstrap.debuggedFlag { return "JIT is on." }
        if jit.method == .stikDebug { return "Husk will use StikDebug." }
        if jit.method == .trollStore { return "Husk will use TrollStore." }
        switch jit.pairingSource {
        case .onDevice: return "Paired on this device."
        case .imported: return "Pairing file imported."
        case nil: return nil
        }
    }

    private var jitPage: some View {
        VStack(spacing: 18) {
            Spacer()
            Image(systemName: "bolt.fill")
                .font(.system(size: 40, weight: .semibold))
                .foregroundStyle(Theme.accent)
                .frame(width: 96, height: 96)
                .background(Theme.accentSoft, in: Circle())
            Text("Turn on JIT").font(.display(32, weight: .heavy))
            Text("Games need JIT, which on iOS only an attached debugger can grant. StikJIT is built into Husk and is "
               + "the recommended way: it turns JIT on from inside the app, with no computer and no other app. "
               + "StikDebug and TrollStore work too.")
                .font(.callout).foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 34)
            if let state = jitState {
                Label(state, systemImage: "checkmark.circle.fill")
                    .font(.callout.weight(.semibold)).foregroundStyle(Theme.good)
            }
            Button(jitState == nil ? "Set Up StikJIT Now" : "Change JIT Setup") { settingUpJIT = true }
                .font(.body.weight(.semibold))
                .foregroundStyle(Theme.accent)
                .padding(.top, 4)
            Spacer()
        }
    }

    private var ready: some View {
        VStack(spacing: 18) {
            Spacer()
            Image(systemName: "square.and.arrow.down.fill")
                .font(.system(size: 38, weight: .semibold))
                .foregroundStyle(Theme.accent)
                .frame(width: 96, height: 96)
                .background(Theme.accentSoft, in: Circle())
            Text("Add your games").font(.display(32, weight: .heavy))
            VStack(alignment: .leading, spacing: 16) {
                feature("plus", "The + button", "Pick an APK or a bundle — .xapk, .apkm or .apks — from Files.")
                feature("square.and.arrow.up", "Share to Husk", "From Files or Safari, share an APK to Husk.")
                feature("bag.fill", "The Store", "Sign in with Google and download games from Google Play.")
            }
            .padding(.horizontal, 32)
            Text("Games need 64-bit (arm64) code. If JIT is off when you press Play, Husk turns it on with the method you "
               + "chose, or walks you through setting one up.")
                .font(.footnote).foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 34)
                .padding(.top, 6)
            Spacer()
        }
    }
}
