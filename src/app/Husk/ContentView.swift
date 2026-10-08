// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

/// The app: three tabs -- the Library, the Store and Settings -- and the sheets that can come up over them.
struct ContentView: View {
    @ObservedObject private var router = Router.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @ObservedObject private var theme = AppTheme.shared
    @ObservedObject private var showcase = ShowcaseStore.shared
    @ObservedObject private var tlStore = TranslationLayerStore.shared
    @ObservedObject private var crash = CrashReport.shared
    @ObservedObject private var updates = AppUpdates.shared
    @State private var showOnboarding = Onboarding.needed
    @State private var showWhatsNew = false
    @Environment(\.scenePhase) private var scenePhase
    @AppStorage(Theme.Appearance.key) private var appearance = Theme.Appearance.system

    var body: some View {
        TabView(selection: $router.tab) {
            LibraryScreen()
                .tabItem { Label("Library", systemImage: "square.grid.2x2.fill") }
                .tag(HuskTab.library)

            StoreView()
                .tabItem { Label("Store", systemImage: "bag.fill") }
                .tag(HuskTab.store)

            SettingsTab()
                .tabItem { Label("Settings", systemImage: "gearshape.fill") }
                .tag(HuskTab.settings)
        }
        .tint(theme.accentColor)
        // The user's appearance: the system's, unless they pinned one. Set on the
        // window rather than with .preferredColorScheme — see Theme.apply.
        .onAppear { Theme.apply(appearance) }
        .onChange(of: appearance) { Theme.apply($0) }
        .fullScreenCover(isPresented: $showOnboarding) {
            OnboardingView { showOnboarding = false }
        }
        // A game that ended Husk last time: say so, with its report.
        .sheet(item: Binding(get: { showOnboarding ? nil : crash.pending }, set: { crash.pending = $0 }),
               onDismiss: { if WhatsNew.due { showWhatsNew = true } }) { r in
            CrashReportSheet(report: r)
        }
        // After an update: what it brought, once.
        .fullScreenCover(isPresented: $showWhatsNew) {
            WhatsNewSheet { WhatsNew.markSeen(); showWhatsNew = false }
        }
        // A newer Husk has been released.
        .alert("Husk \(updates.available?.version ?? "") Is Available", isPresented: Binding(
                get: { updates.available != nil && !showOnboarding && !showWhatsNew && crash.pending == nil },
                set: { if !$0 { updates.dismiss() } })) {
            Button("View Release") {
                if let page = updates.available?.page { UIApplication.shared.open(page) }
                updates.dismiss()
            }
            Button("Not Now", role: .cancel) { updates.dismiss() }
        } message: {
            Text("You have \(AppUpdates.current). The new version's IPA is on its release page.")
        }
        .task { await updates.check() }
        // Pictures for the games here: read the index on launch and whenever the set of games changes.
        .task(id: showcasePackages) { showcase.refresh(for: showcasePackages) }
        .alert("Metadata Updated", isPresented: Binding(get: { showcase.shouldAsk && !showOnboarding },
                                                         set: { if !$0 { showcase.asked() } })) {
            Button("Download") { showcase.downloadUpdates() }
            Button("Not Now", role: .cancel) { showcase.asked() }
        } message: {
            let n = showcase.updates.count
            Text("Pictures for \(n) of your game\(n == 1 ? "" : "s") have been updated. Would you like to download them?")
        }
        .sheet(isPresented: $jit.showSetup) { JITSetupFlow() }
        // The built-in helper attaches while Husk stays in the foreground, so there is no relaunch to trigger the region
        // claim below; this is it.
        .onChange(of: jit.attachGeneration) { _ in evaluate() }
        .onAppear {
            crash.checkPreviousRun()
            if crash.pending == nil { router.resumeSwitch() }
            if crash.pending == nil, WhatsNew.due { showWhatsNew = true }
            evaluate()
        }
        // The two-parameter onChange is iOS 17; this single-parameter form is
        // deprecated there but still works, and is the only one that compiles
        // against the 16.4 deployment target.
        .onChange(of: scenePhase) { phase in
            // StikDebug relaunches Husk after attaching, so returning to the
            // foreground is the moment worth re-checking, not first launch.
            if phase == .active { evaluate() }
        }
    }

    /// Every package Husk holds: what the picture index is read for.
    private var showcasePackages: Set<String> {
        Set(tlStore.apps.compactMap(\.packageName))
    }

    private func evaluate() {
        // Before anything else wants JIT: the setting that turns it on as Husk opens.
        jit.enableAtLaunchIfAsked()
        // APKs put straight into Husk's folder in the Files app become games.
        tlStore.adoptDroppedAPKs()

        guard JITBootstrap.isDebuggerAttached else {
            HuskLog.log("ui", "no debugger attached; JIT is off")
            return
        }

        // Claim the JIT region now, on the first foreground pass after the debugger attaches, while it is still running:
        // the translation layer places every game's code in it, and a debugger that iOS has since suspended cannot grant
        // another. After the first call this is a no-op, and on success it also detaches the debugger.
        JITBootstrap.prewarm()
    }
}

/// Husk's live log, with a way to share it. The share sheet is the practical way to get husk.log off the device.
struct LogView: View {
    var isSheet = true
    @Environment(\.dismiss) private var dismiss
    @State private var lines: [String] = []
    @State private var showShare = false
    private let tick = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some View {
        NavigationStack {
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 1) {
                        ForEach(Array(lines.enumerated()), id: \.offset) { i, line in
                            Text(line)
                                .font(.system(size: 10, design: .monospaced))
                                .textSelection(.enabled)
                                .foregroundStyle(color(for: line))
                                .frame(maxWidth: .infinity, alignment: .leading)
                                .id(i)
                        }
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 8)
                }
                .background(Color(white: 0.06).ignoresSafeArea())
                .onReceive(tick) { _ in
                    lines = HuskLog.recentLines(800)
                    if let last = lines.indices.last {
                        proxy.scrollTo(last, anchor: .bottom)
                    }
                }
            }
            .navigationTitle("Console")
            .navigationBarTitleDisplayMode(.inline)
            .toolbarBackground(.visible, for: .navigationBar)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    if isSheet {
                        Button("Done") { dismiss() }
                    }
                }
                ToolbarItem(placement: .primaryAction) {
                    Button { showShare = true } label: { Image(systemName: "square.and.arrow.up") }
                        .accessibilityLabel("Share Logs")
                }
            }
            .sheet(isPresented: $showShare) {
                ShareSheet(items: [
                    HuskLog.logFileURL,
                    // The native runtime's own output: what a crash is diagnosed from.
                    HuskLog.nativeLogURL,
                    HuskLog.previousNativeLogURL,
                ])
            }
        }
        .environment(\.colorScheme, .dark)
    }

    /// Colour by source so the JIT path and the runtime stand out.
    private func color(for line: String) -> Color {
        if line.contains("FAIL") || line.contains("FATAL") || line.contains("error") {
            return Color(red: 1, green: 0.42, blue: 0.4)
        }
        if line.contains("[husk-jit]") || line.contains("[jit]") { return .green }
        if line.contains("[tl]") { return .cyan }
        return .white.opacity(0.85)
    }
}

struct ShareSheet: UIViewControllerRepresentable {
    let items: [Any]
    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: items, applicationActivities: nil)
    }
    func updateUIViewController(_ vc: UIActivityViewController, context: Context) {}
}
