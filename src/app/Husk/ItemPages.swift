// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

// MARK: - pieces of a page

/// A row on a page's card that goes somewhere or does something: a coloured icon, a title, and a chevron when it opens a page.
private struct PageRow: View {
    let systemImage: String
    let tint: Color
    let title: String
    var detail: String? = nil
    var chevron = true
    var destructive = false

    var body: some View {
        HStack(spacing: 14) {
            SettingsIcon(systemImage: systemImage, tint: tint)
            Text(title)
                .font(.system(size: 16))
                .foregroundStyle(destructive ? Theme.bad : Theme.text)
            Spacer(minLength: 8)
            if let detail {
                Text(detail).font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
            }
            if chevron {
                Image(systemName: "chevron.right")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(.tertiary)
            }
        }
        .padding(.horizontal, 16)
        .frame(minHeight: 52)
        .contentShape(Rectangle())
    }
}

/// A titled card of rows.
private struct PageCard<Content: View>: View {
    var title: String? = nil
    var footer: String? = nil
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if let title {
                Text(title.uppercased())
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(.secondary)
                    .padding(.leading, 16)
            }
            VStack(spacing: 0) { content }
                .huskCard()
            if let footer {
                Text(footer)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, 16)
            }
        }
    }
}

private func fileBytes(_ paths: [String]) -> String {
    let total = paths.reduce(Int64(0)) { sum, p in
        sum + (((try? FileManager.default.attributesOfItem(atPath: p)[.size]) as? NSNumber)?.int64Value ?? 0)
    }
    return ByteCountFormatter.string(fromByteCount: total, countStyle: .file)
}

@MainActor private func turnOnJIT() {
    let jit = JITCoordinator.shared
    if HuskBuiltInJIT.isAvailable { jit.method = .builtIn }
    jit.enable()
}

// MARK: - a game

/// One game: Play, its settings, its saves, what it is, and removing it.
struct GamePage: View {
    let app: TLApp

    @ObservedObject private var showcase = ShowcaseStore.shared
    @ObservedObject private var store = TranslationLayerStore.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @ObservedObject private var statuses = GameStatusStore.shared
    @AppStorage(TranslationLayer.devInfoKey) private var devInfo = false
    @Environment(\.dismiss) private var dismiss
    @State private var playing = false
    @State private var confirmRemove = false
    @State private var backupFile: URL?
    @State private var savesBusy = false
    @State private var savesMessage: String?
    @State private var confirmRestore: URL?
    @State private var confirmQuit = false

    /// Another game already loaded in this run of Husk: engines cannot be unloaded, so this one cannot start until Husk is
    /// closed and opened again.
    private var blockedBy: String? {
        guard let loaded = husk_native_loaded_apk().map({ String(cString: $0) }), let apk = app.apks.first, loaded != apk
        else { return nil }
        return store.apps.first { $0.apks.first == loaded }?.label ?? "Another game"
    }

    /// This game's engine is loaded in this run of Husk, so its files may be open: no restoring under it.
    private var loadedNow: Bool {
        guard let loaded = husk_native_loaded_apk().map({ String(cString: $0) }) else { return false }
        return loaded == app.apks.first
    }

    private var geometryDash: TLApp? { store.apps.first { $0.packageName == GeodeSupport.gamePackage } }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 22) {
                VStack(spacing: 14) {
                    hero
                    primary
                    if let note { Text(note).font(.footnote).foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .leading).fixedSize(horizontal: false, vertical: true)
                        .padding(.horizontal, 4) }
                }

                if !showcase.pictures(for: app.packageName).isEmpty {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("SCREENSHOTS")
                            .font(.system(size: 12, weight: .semibold)).foregroundStyle(.secondary).padding(.leading, 16)
                        ShowcaseGallery(package: app.packageName)
                    }
                }

                PageCard {
                    NavigationLink(value: LibraryRoute.gameSettings(app.id)) {
                        PageRow(systemImage: "slider.horizontal.3", tint: .blue, title: "Game Settings")
                    }
                    .buttonStyle(.plain)
                    RowDivider(inset: 59)
                    NavigationLink(value: LibraryRoute.gameReport(app.id)) {
                        PageRow(systemImage: "cpu", tint: .gray, title: "Technical Details")
                    }
                    .buttonStyle(.plain)
                }

                if !app.isGeodeLauncher {
                    PageCard(title: "Saves", footer: savesFooter) {
                        Button { backUp() } label: {
                            PageRow(systemImage: "square.and.arrow.up", tint: .green, title: "Back Up Saves",
                                    chevron: false)
                        }
                        .buttonStyle(.plain)
                        .disabled(savesBusy || !SaveBackup.hasData(app))
                        .opacity(savesBusy || !SaveBackup.hasData(app) ? 0.45 : 1)
                        RowDivider(inset: 59)
                        Button { pickRestore() } label: {
                            PageRow(systemImage: "clock.arrow.circlepath", tint: .orange, title: "Restore Saves",
                                    chevron: false)
                        }
                        .buttonStyle(.plain)
                        .disabled(savesBusy || loadedNow)
                        .opacity(savesBusy || loadedNow ? 0.45 : 1)
                    }
                }

                PageCard(title: "About") {
                    VStack(spacing: 12) {
                        DetailRow(label: "Runs with", value: app.report?.runnerName ?? "Unknown", mono: false)
                        if let st = statuses.status(app.id) {
                            HStack {
                                Text("Last result").foregroundStyle(Theme.textDim)
                                Spacer()
                                Label(st.result.label, systemImage: st.result.symbol)
                                    .foregroundStyle(st.result.color)
                            }
                            .font(.system(size: 15))
                        }
                        DetailRow(label: "Last played",
                                  value: app.lastPlayed.map { $0.formatted(date: .abbreviated, time: .shortened) } ?? "Never",
                                  mono: false)
                        DetailRow(label: "Size", value: fileBytes(app.apks), mono: false)
                        if let package = app.packageName {
                            DetailRow(label: "Package", value: package)
                        }
                    }
                    .padding(16)
                }

                PageCard {
                    Button { confirmRemove = true } label: {
                        PageRow(systemImage: "trash.fill", tint: .red, title: "Remove Game", chevron: false,
                                destructive: true)
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal, Theme.margin)
            .padding(.top, 8)
            .padding(.bottom, 36)
        }
        .scrollIndicators(.hidden)
        .background(Theme.bg.ignoresSafeArea())
        .navigationTitle(app.label)
        .navigationBarTitleDisplayMode(.inline)
        .fullScreenCover(isPresented: $playing) { TLAttemptView(app: app) }
        .sheet(item: Binding(get: { backupFile.map(IdentifiedURL.init) }, set: { backupFile = $0?.url })) { f in
            ShareSheet(items: [f.url])
        }
        .confirmationDialog("Replace \(app.label)'s saves?", isPresented: Binding(get: { confirmRestore != nil }, set: { if !$0 { confirmRestore = nil } }),
                            titleVisibility: .visible) {
            Button("Replace", role: .destructive) { if let url = confirmRestore { restore(url) } }
        } message: {
            Text("What \(app.label) has saved now is replaced by the backup. Back up first if you want to keep it.")
        }
        .confirmationDialog("Close Husk?", isPresented: $confirmQuit, titleVisibility: .visible) {
            Button("Close Husk") {
                // The game that is loaded is closed with Husk, which is the normal way out for it, not a crash; and this
                // one starts by itself when Husk is opened again.
                Router.shared.switchTo(app.id)
                CrashReport.gameEnded()
                HuskLog.flushNow()
                exit(0)
            }
        } message: {
            Text("\(blockedBy ?? "The other game") is still loaded and only one game can run per session. Husk closes; open it again and \(app.label) starts by itself.")
        }
        .confirmationDialog("Remove \(app.label)?", isPresented: $confirmRemove, titleVisibility: .visible) {
            Button("Remove Game", role: .destructive) {
                dismiss()
                store.remove(app)
            }
        } message: {
            Text("The game and everything it saved in Husk are deleted.")
        }
        .id(jit.attachGeneration)
        .onAppear {
            // Kept until JIT is on: this page is rebuilt when it attaches (.id above), and plays then.
            guard Router.shared.autoPlay == app.id, Launcher.jitOn, blockedBy == nil else { return }
            Router.shared.autoPlay = nil
            play()
        }
    }

    // MARK: header

    private var hero: some View {
        ZStack(alignment: .bottomLeading) {
            GameArtwork(app: app, pixels: 1200)
            LinearGradient(colors: [.clear, .black.opacity(0.72)], startPoint: .top, endPoint: .bottom)
            HStack(alignment: .bottom, spacing: 14) {
                GameIcon(app: app, size: 84)
                    .shadow(color: .black.opacity(0.35), radius: 12, y: 5)
                VStack(alignment: .leading, spacing: 6) {
                    Text(app.label)
                        .font(.display(24, weight: .heavy))
                        .lineLimit(2)
                        .minimumScaleFactor(0.8)
                        .foregroundStyle(.white)
                    HStack(spacing: 6) {
                        Text(app.engineLabel)
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(.white)
                            .padding(.horizontal, 9).padding(.vertical, 4)
                            .background(.white.opacity(0.2), in: Capsule())
                        if let played = app.playedText {
                            Text(played).font(.caption).foregroundStyle(.white.opacity(0.75)).lineLimit(1)
                        }
                    }
                }
                Spacer(minLength: 0)
            }
            .padding(16)
        }
        .frame(height: 230)
        .clipShape(RoundedRectangle(cornerRadius: Theme.heroCorner, style: .continuous))
    }

    /// What to say under the button, when there is something.
    private var note: String? {
        if app.isGeodeLauncher {
            return "This is Geode's Android launcher. On Husk, Geode runs inside Geometry Dash instead: turn it on here (or in "
                 + "Geometry Dash's settings), then start Geometry Dash and use Geode's button on its main menu to get mods. "
                 + "This APK supplies a library Geode needs, so keep it."
        }
        if let other = blockedBy {
            return "\(other) is still loaded, and a game cannot be unloaded once it has started. Close Husk with the button "
                 + "above, then open it again to play \(app.label)."
        }
        if !app.runs { return "This APK has no 64-bit code Husk can run, so it will probably not start." }
        return nil
    }

    private var savesFooter: String {
        if let savesMessage { return savesMessage }
        if loadedNow { return "\(app.label) has been started in this run of Husk. Close Husk and open it again to restore saves." }
        return "A backup is a .zip of everything \(app.label) has saved in Husk, to keep in Files or move to another device."
    }

    @ViewBuilder
    private var primary: some View {
        if app.isGeodeLauncher {
            if let gd = geometryDash {
                let on = TLAppSettings.load(gd.id).geode
                Button {
                    var s = TLAppSettings.load(gd.id)
                    s.geode = true
                    s.save(gd.id)
                    Task { await GeodeSupport.shared.prepare(gd) }
                } label: {
                    Label(on ? "Geode Is On for Geometry Dash" : "Turn On Geode for Geometry Dash",
                          systemImage: "puzzlepiece.extension.fill")
                }
                .buttonStyle(PrimaryButtonStyle(enabled: !on))
                .disabled(on)
            } else {
                Button {} label: { Label("Add Geometry Dash First", systemImage: "plus") }
                    .buttonStyle(PrimaryButtonStyle(enabled: false))
                    .disabled(true)
            }
        } else if blockedBy != nil {
            Button { confirmQuit = true } label: { Label("Close Husk to Play", systemImage: "arrow.clockwise") }
                .buttonStyle(SecondaryButtonStyle())
        } else if !Launcher.jitOn {
            if jit.busy {
                HStack(spacing: 10) { ProgressView(); Text(jit.status ?? "Turning on JIT…").foregroundStyle(.secondary) }
                    .frame(maxWidth: .infinity).frame(height: 52)
            } else {
                Button { turnOnJIT() } label: { Label("Turn On JIT to Play", systemImage: "bolt.fill") }
                    .buttonStyle(PrimaryButtonStyle())
            }
        } else {
            Button { play() } label: { Label(app.runs ? "Play" : "Try to Run", systemImage: "play.fill") }
                .buttonStyle(PrimaryButtonStyle())
        }
    }

    // MARK: actions

    /// Play: with Geode on, whatever it is missing (a newer release, its resources) is fetched first.
    private func play() {
        guard TLAppSettings.load(app.id).geode, case .ready = GeodeSupport.shared.current(app) else {
            if TLAppSettings.load(app.id).geode {
                Task { await GeodeSupport.shared.prepare(app); playing = true }
            } else {
                playing = true
            }
            return
        }
        playing = true
    }

    private func backUp() {
        savesBusy = true; savesMessage = nil
        let app = self.app
        Task.detached {
            let result = Result { try SaveBackup.export(app) }
            await MainActor.run {
                savesBusy = false
                switch result {
                case .success(let url): backupFile = url
                case .failure(let e): savesMessage = e.localizedDescription
                }
            }
        }
    }

    private func pickRestore() {
        HuskFilePicker.present(types: [.zip], multiple: false) { urls in
            if let url = urls.first { confirmRestore = url }
        } onFail: { savesMessage = $0 }
    }

    private func restore(_ url: URL) {
        savesBusy = true; savesMessage = nil
        let app = self.app
        Task.detached {
            let result = Result { try SaveBackup.restore(app, from: url) }
            await MainActor.run {
                savesBusy = false
                switch result {
                case .success(let n): savesMessage = "Restored \(n) file\(n == 1 ? "" : "s") from \(url.lastPathComponent)."
                case .failure(let e): savesMessage = e.localizedDescription
                }
            }
        }
    }
}

/// A URL that a sheet can be presented for.
struct IdentifiedURL: Identifiable {
    let url: URL
    var id: String { url.path }
}
