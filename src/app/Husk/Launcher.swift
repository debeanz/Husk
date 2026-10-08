// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

// MARK: - what the library knows about a game

extension TLApp {
    /// "Played 2 hours ago", or nothing for a game never started from Husk.
    var playedText: String? {
        guard let date = lastPlayed else { return nil }
        if date.timeIntervalSinceNow > -60 { return "Played just now" }
        let rel = RelativeDateTimeFormatter()
        rel.unitsStyle = .full
        return "Played " + rel.localizedString(for: date, relativeTo: Date())
    }

    /// What runs it, in a word or two: the engine, Husk's Java interpreter, or nothing.
    var engineLabel: String {
        guard let report else { return "Not scanned" }
        if report.runsOnNativeRuntime { return report.nativeEngineName }
        return report.canRun ? "Java" : "Unsupported"
    }

    /// Whether Husk expects it to start at all.
    var runs: Bool { report?.canRun == true }

    /// Geode's Android launcher, added as if it were a game: on Husk it only lends Geometry Dash a library.
    var isGeodeLauncher: Bool { packageName == GeodeSupport.launcherPackage }

    /// A gameplay picture, for games that have been run on an iPhone (fetched from GitHub; see ShowcaseStore).
    @MainActor var artworkPath: String? { ShowcaseStore.shared.pictures(for: packageName).first }
}

/// How the library is ordered.
enum LibrarySort: String, CaseIterable, Identifiable {
    case recent, name
    var id: String { rawValue }
    var title: String { self == .recent ? "Recently Played" : "Name" }
}

@MainActor
enum Launcher {
    /// Most recently played first; never-played ones after, by name.
    static func byRecent(_ apps: [TLApp]) -> [TLApp] {
        apps.sorted { a, b in
            switch (a.lastPlayed, b.lastPlayed) {
            case let (x?, y?): return x > y
            case (_?, nil): return true
            case (nil, _?): return false
            default: return a.label.localizedCaseInsensitiveCompare(b.label) == .orderedAscending
            }
        }
    }

    static var jitOn: Bool { JITBootstrap.isDebuggerAttached || JITBootstrap.debuggedFlag }
}

// MARK: - the Library

/// Every game, as artwork: the one you played last up top, then all of them in a grid.
struct LibraryScreen: View {
    @ObservedObject private var router = Router.shared
    @ObservedObject private var store = TranslationLayerStore.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @ObservedObject private var incoming = IncomingFiles.shared
    @ObservedObject private var showcase = ShowcaseStore.shared
    @AppStorage("husk.library.sort") private var sort: LibrarySort = .recent
    @State private var query = ""
    @State private var removing: TLApp?

    private let columns = [GridItem(.adaptive(minimum: 150, maximum: 220), spacing: 14, alignment: .top)]

    private var shown: [TLApp] {
        let found = store.apps.filter { matches(query, $0.label, $0.packageName ?? "") }
        switch sort {
        case .recent: return Launcher.byRecent(found)
        case .name: return found.sorted { $0.label.localizedCaseInsensitiveCompare($1.label) == .orderedAscending }
        }
    }

    /// The game to pick up where you left off: the last one played, when there is one.
    private var lead: TLApp? {
        guard query.isEmpty else { return nil }
        return Launcher.byRecent(store.apps).first { $0.lastPlayed != nil && !$0.isGeodeLauncher }
    }

    var body: some View {
        NavigationStack(path: $router.library) {
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    header

                    if !Launcher.jitOn { JITCard(compact: true) }
                    if let busy = incoming.preparing { BusyStrip(text: busy) }
                    if let busy = store.busy { BusyStrip(text: busy) }
                    if jit.busy, !jit.showSetup { BusyStrip(text: jit.status ?? "Turning on JIT…") }
                    if let error = store.lastError { ErrorStrip(text: error) { store.lastError = nil } }

                    if store.apps.isEmpty {
                        WelcomeCard()
                    } else {
                        if let lead { ContinueCard(app: lead) }
                        if store.apps.count > 4 { SearchField(text: $query, prompt: "Search your games") }
                        gridHeader
                        let apps = shown
                        if apps.isEmpty {
                            NoResults(query: query)
                        } else {
                            LazyVGrid(columns: columns, spacing: 20) {
                                ForEach(apps) { app in
                                    NavigationLink(value: LibraryRoute.game(app.id)) { GameTile(app: app) }
                                        .buttonStyle(CardButtonStyle())
                                        .contextMenu { menu(for: app) }
                                }
                            }
                        }
                    }
                }
                .padding(.horizontal, Theme.margin)
                .padding(.top, 8)
                .padding(.bottom, 36)
                .id(jit.attachGeneration)
            }
            .scrollIndicators(.hidden)
            .scrollDismissesKeyboard(.immediately)
            .background(Theme.canvas.ignoresSafeArea())
            .toolbar(.hidden, for: .navigationBar)
            .libraryDestinations()
            .confirmationDialog("Remove \(removing?.label ?? "this game")?",
                                isPresented: Binding(get: { removing != nil }, set: { if !$0 { removing = nil } }),
                                titleVisibility: .visible) {
                Button("Remove Game", role: .destructive) {
                    if let app = removing { store.remove(app) }
                    removing = nil
                }
            } message: {
                Text("The game and everything it saved in Husk are deleted.")
            }
        }
    }

    private var header: some View {
        HStack(alignment: .center, spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text("Library").font(.display(34, weight: .heavy))
                Text(store.apps.isEmpty ? "Android games, running natively"
                     : "\(store.apps.count) game\(store.apps.count == 1 ? "" : "s")")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Button { router.addGames() } label: {
                Image(systemName: "plus")
                    .font(.system(size: 17, weight: .bold))
                    .foregroundStyle(.white)
                    .frame(width: 40, height: 40)
                    .background(Theme.accent, in: Circle())
            }
            .buttonStyle(CardButtonStyle())
            .accessibilityLabel("Add a Game")
        }
        .padding(.top, 6)
    }

    private var gridHeader: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(query.isEmpty ? "All Games" : "Results")
                .font(.display(22))
            Spacer()
            Menu {
                Picker("Sort By", selection: $sort) {
                    ForEach(LibrarySort.allCases) { Text($0.title).tag($0) }
                }
            } label: {
                HStack(spacing: 4) {
                    Text(sort.title)
                    Image(systemName: "chevron.up.chevron.down").font(.system(size: 10, weight: .bold))
                }
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(Theme.accent)
            }
        }
        .padding(.top, 4)
    }

    @ViewBuilder
    private func menu(for app: TLApp) -> some View {
        Button {
            router.autoPlay = app.id
            router.library.append(.game(app.id))
        } label: { Label("Play", systemImage: "play.fill") }
            .disabled(app.isGeodeLauncher)
        Button { router.library.append(.gameSettings(app.id)) } label: {
            Label("Game Settings", systemImage: "slider.horizontal.3")
        }
        Divider()
        Button(role: .destructive) { removing = app } label: { Label("Remove", systemImage: "trash") }
    }
}

// MARK: - the cards

/// What sits behind a game's name: its gameplay picture when Husk has one, its icon enlarged and blurred otherwise.
struct GameArtwork: View {
    let app: TLApp
    var pixels: Int = 800
    @ObservedObject private var showcase = ShowcaseStore.shared

    var body: some View {
        Group {
            if let art = app.artworkPath {
                Color.clear.overlay { PictureView(path: art, pixels: pixels) }.clipped()
            } else {
                ZStack {
                    Color(uiColor: .secondarySystemBackground)
                    if app.iconPath != nil {
                        AppIcon(path: app.iconPath, size: 120)
                            .scaleEffect(3.2)
                            .blur(radius: 30)
                            .saturation(1.4)
                            .opacity(0.9)
                    }
                }
                .clipped()
            }
        }
        // Scaled up, the icon reaches far past the card; clipping hides that but does not stop it taking touches.
        .allowsHitTesting(false)
    }
}

/// A game's icon, with the hairline edge an icon has on a home screen.
struct GameIcon: View {
    let app: TLApp
    let size: CGFloat

    var body: some View {
        AppIcon(path: app.iconPath, size: size)
            .overlay {
                RoundedRectangle(cornerRadius: size * 0.225, style: .continuous)
                    .strokeBorder(Color.white.opacity(0.18), lineWidth: 0.5)
            }
    }
}

/// The big card up top: the game you played last, ready to go again. The card opens the game's page; Play starts it.
private struct ContinueCard: View {
    let app: TLApp
    @ObservedObject private var router = Router.shared

    var body: some View {
        // The Play button sits over the card rather than inside it: a button inside a navigation link's label does not
        // reliably get its own taps.
        ZStack(alignment: .bottomTrailing) {
            NavigationLink(value: LibraryRoute.game(app.id)) {
                ZStack(alignment: .bottomLeading) {
                    GameArtwork(app: app, pixels: 1200)
                    LinearGradient(colors: [.black.opacity(0.05), .black.opacity(0.78)], startPoint: .top, endPoint: .bottom)
                    VStack(alignment: .leading, spacing: 0) {
                        Text("CONTINUE PLAYING")
                            .font(.system(size: 11, weight: .bold, design: .rounded))
                            .tracking(0.8)
                            .foregroundStyle(.white.opacity(0.7))
                        Spacer(minLength: 0)
                        HStack(alignment: .bottom, spacing: 14) {
                            GameIcon(app: app, size: 60)
                                .shadow(color: .black.opacity(0.35), radius: 10, y: 4)
                            VStack(alignment: .leading, spacing: 3) {
                                Text(app.label)
                                    .font(.display(21, weight: .heavy))
                                    .lineLimit(2)
                                    .minimumScaleFactor(0.85)
                                Text(app.playedText ?? app.engineLabel)
                                    .font(.caption)
                                    .foregroundStyle(.white.opacity(0.75))
                            }
                            .foregroundStyle(.white)
                            Spacer(minLength: 0)
                        }
                        // Room for the Play button beside the name.
                        .padding(.trailing, 92)
                    }
                    .padding(16)
                }
                .frame(height: 210)
                .clipShape(RoundedRectangle(cornerRadius: Theme.heroCorner, style: .continuous))
                .contentShape(RoundedRectangle(cornerRadius: Theme.heroCorner, style: .continuous))
            }
            .buttonStyle(CardButtonStyle())

            Button {
                router.autoPlay = app.id
                router.library.append(.game(app.id))
            } label: {
                Label("Play", systemImage: "play.fill")
                    .font(.system(size: 15, weight: .bold, design: .rounded))
                    .foregroundStyle(.black)
                    .padding(.horizontal, 16)
                    .frame(height: 38)
                    .background(.white, in: Capsule())
            }
            .buttonStyle(CardButtonStyle())
            .padding(16)
        }
    }
}

/// One game in the grid: its artwork with the icon on it, its name, and how it runs.
struct GameTile: View {
    let app: TLApp
    @ObservedObject private var statuses = GameStatusStore.shared

    private var result: GameStatusStore.Result? { statuses.status(app.id)?.result }

    var body: some View {
        VStack(alignment: .leading, spacing: 9) {
            ZStack {
                GameArtwork(app: app, pixels: 520)
                LinearGradient(colors: [.clear, .black.opacity(0.28)], startPoint: .center, endPoint: .bottom)
                GameIcon(app: app, size: 68)
                    .shadow(color: .black.opacity(0.3), radius: 10, y: 4)
            }
            .aspectRatio(1, contentMode: .fit)
            .clipShape(RoundedRectangle(cornerRadius: Theme.tileCorner, style: .continuous))
            .contentShape(RoundedRectangle(cornerRadius: Theme.tileCorner, style: .continuous))
            .overlay(alignment: .topTrailing) {
                if let result { GameStatusBadge(result: result).padding(9) }
            }

            VStack(alignment: .leading, spacing: 2) {
                Text(app.label)
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(.primary)
                    .lineLimit(1)
                Text(caption)
                    .font(.caption)
                    .foregroundStyle(app.runs || app.isGeodeLauncher ? Color.secondary : Theme.warn)
                    .lineLimit(1)
            }
            .padding(.horizontal, 2)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .contentShape(Rectangle())
    }

    private var caption: String {
        if app.isGeodeLauncher { return "Geode" }
        if !app.runs { return "May not run" }
        if let played = app.playedText { return "\(app.engineLabel) · \(played.replacingOccurrences(of: "Played ", with: ""))" }
        return app.engineLabel
    }
}

/// The first time: nothing added yet, and the ways to start.
private struct WelcomeCard: View {
    @ObservedObject private var router = Router.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Image(systemName: "gamecontroller.fill")
                .font(.system(size: 26, weight: .semibold))
                .foregroundStyle(Theme.accent)
                .frame(width: 58, height: 58)
                .background(Theme.accentSoft, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
            VStack(alignment: .leading, spacing: 6) {
                Text("Add your first game")
                    .font(.display(24))
                Text("Pick an APK or a bundle (.xapk, .apkm, .apks), share one to Husk from Files or Safari, "
                   + "or get one from the Store. Games run straight on your iPhone — no Android to boot.")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            VStack(spacing: 10) {
                Button { router.addGames() } label: { Label("Add a Game", systemImage: "plus") }
                    .buttonStyle(PrimaryButtonStyle())
                Button { router.tab = .store } label: { Label("Browse the Store", systemImage: "bag") }
                    .buttonStyle(SecondaryButtonStyle())
            }
        }
        .padding(20)
        .huskCard(RoundedRectangle(cornerRadius: Theme.heroCorner, style: .continuous))
    }
}

/// Something that went wrong, said once, with a way to put it away.
private struct ErrorStrip: View {
    let text: String
    let close: () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Theme.warn)
            Text(text).font(.subheadline).fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
            Button(action: close) {
                Image(systemName: "xmark").font(.system(size: 12, weight: .bold)).foregroundStyle(.secondary)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Dismiss")
        }
        .padding(14)
        .huskCard(RoundedRectangle(cornerRadius: Theme.rowCorner, style: .continuous))
    }
}

// MARK: - shared pieces

/// A search field as the system draws one.
struct SearchField: View {
    @Binding var text: String
    let prompt: String
    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: 8) {
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass")
                    .font(.system(size: 15, weight: .medium))
                    .foregroundStyle(.secondary)
                TextField(prompt, text: $text)
                    .focused($focused)
                    .submitLabel(.search)
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                if !text.isEmpty {
                    Button { text = "" } label: {
                        Image(systemName: "xmark.circle.fill").foregroundStyle(.tertiary)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Clear")
                }
            }
            .padding(.horizontal, 12)
            .frame(height: 40)
            .background(Color(uiColor: .tertiarySystemFill), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            if focused {
                Button("Cancel") { text = ""; focused = false }
                    .transition(.move(edge: .trailing).combined(with: .opacity))
            }
        }
        .animation(.easeOut(duration: 0.2), value: focused)
    }
}

/// Nothing matches the search.
struct NoResults: View {
    let query: String
    var body: some View {
        VStack(spacing: 8) {
            Image(systemName: "magnifyingglass").font(.system(size: 28, weight: .light)).foregroundStyle(.secondary)
            Text("No Results").font(.headline)
            Text("No game is called “\(query)”.").font(.subheadline).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 48)
    }
}

/// Whether a name matches what was searched for.
func matches(_ query: String, _ names: String...) -> Bool {
    let q = query.trimmingCharacters(in: .whitespaces)
    return q.isEmpty || names.contains { $0.localizedCaseInsensitiveContains(q) }
}

extension View {
    /// The pages a game opens.
    func libraryDestinations() -> some View {
        navigationDestination(for: LibraryRoute.self) { route in
            LibraryRouteView(route: route).toolbar(.visible, for: .navigationBar)
        }
    }
}

private struct LibraryRouteView: View {
    let route: LibraryRoute
    @ObservedObject private var store = TranslationLayerStore.shared

    var body: some View {
        switch route {
        case .game(let id):
            if let app = store.apps.first(where: { $0.id == id }) { GamePage(app: app) } else { GoneView() }
        case .gameSettings(let id):
            if let app = store.apps.first(where: { $0.id == id }) { TLAppSettingsView(app: app) } else { GoneView() }
        case .gameReport(let id):
            if let app = store.apps.first(where: { $0.id == id }) { TLTechnicalView(app: app) } else { GoneView() }
        }
    }
}

private struct GoneView: View {
    var body: some View {
        Text("This game was removed.").foregroundStyle(.secondary).frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
