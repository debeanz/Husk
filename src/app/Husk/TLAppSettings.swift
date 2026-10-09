// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

/// What a person can change about how one game runs here, kept in the game's own folder (`settings.json`) so it goes when the game
/// does. Like LiveContainer's per-app settings: each game has its own, and nothing here touches another game.
struct TLAppSettings: Codable, Equatable {
    /// Which way up the screen is while the game runs.
    enum Orientation: String, Codable, CaseIterable, Identifiable {
        /// What the game asks for (its manifest), landscape when it does not say.
        case auto, landscape, portrait
        var id: String { rawValue }
        var title: String {
            switch self {
            case .auto: return "Automatic"
            case .landscape: return "Landscape"
            case .portrait: return "Portrait"
            }
        }
    }

    /// When the on-screen controller is offered.
    enum PadMode: String, Codable, CaseIterable, Identifiable {
        /// For the games that cannot be played without a controller (Unreal), when none is connected.
        case auto, always, never
        var id: String { rawValue }
        var title: String {
            switch self {
            case .auto: return "Automatic"
            case .always: return "Always"
            case .never: return "Never"
            }
        }
    }

    var orientation: Orientation = .auto
    /// The size the game draws at: "default" (Settings'), "auto", "screen", or a fixed "1920x1080" (GameDisplay).
    var resolution: String = GameDisplay.followDefault
    /// How the game's picture fills the screen: "default" (Settings'), or a GameScaling.
    var scaling: String = GameDisplay.followDefault
    /// The frame rate the game is held to: "default" (Settings'), "60" or "120" (GameDisplay).
    var frameRate: String = GameDisplay.followDefault
    /// Full screen: the game draws over the whole display, the area around the camera included. Off, it keeps clear of the
    /// camera. (Stored under its old name, from when it was "Hide the interface", so the choice carries over.)
    var cleanView = false
    /// Stop the screen from dimming and locking while the game is on.
    var keepAwake = true
    var pad: PadMode = .auto
    /// Whether the controller is showing right now (the Pad button in the top bar toggles it).
    var padShown = true
    var padOpacity = 1.0
    var haptics = true
    /// Geometry Dash only: load Geode, its mod loader (Geode.swift).
    var geode = false

    init() {}

    // Settings written by an older Husk lack the keys a newer one added; each falls back to its default.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        orientation = try c.decodeIfPresent(Orientation.self, forKey: .orientation) ?? .auto
        // Older settings stored a density: low, medium, high (the default) or native. Native is the screen's own pixels;
        // the others follow the new default, Automatic, which is what high was.
        switch try c.decodeIfPresent(String.self, forKey: .resolution) {
        case "native"?: resolution = GameDisplay.screen
        case let r? where GameDisplay.resolutions.contains(r): resolution = r
        default: resolution = GameDisplay.followDefault
        }
        scaling = try c.decodeIfPresent(String.self, forKey: .scaling) ?? GameDisplay.followDefault
        frameRate = try c.decodeIfPresent(String.self, forKey: .frameRate) ?? GameDisplay.followDefault
        cleanView = try c.decodeIfPresent(Bool.self, forKey: .cleanView) ?? false
        keepAwake = try c.decodeIfPresent(Bool.self, forKey: .keepAwake) ?? true
        pad = try c.decodeIfPresent(PadMode.self, forKey: .pad) ?? .auto
        padShown = try c.decodeIfPresent(Bool.self, forKey: .padShown) ?? true
        padOpacity = try c.decodeIfPresent(Double.self, forKey: .padOpacity) ?? 1.0
        haptics = try c.decodeIfPresent(Bool.self, forKey: .haptics) ?? true
        geode = try c.decodeIfPresent(Bool.self, forKey: .geode) ?? false
    }

    private static func url(_ id: String) -> URL {
        TranslationLayer.root.appendingPathComponent(id, isDirectory: true).appendingPathComponent("settings.json")
    }

    static func load(_ id: String) -> TLAppSettings {
        (try? Data(contentsOf: url(id))).flatMap { try? JSONDecoder().decode(TLAppSettings.self, from: $0) } ?? TLAppSettings()
    }

    func save(_ id: String) {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        // The folder is gone when the app was removed while its settings page was open.
        guard FileManager.default.fileExists(atPath: Self.url(id).deletingLastPathComponent().path),
              let data = try? encoder.encode(self) else { return }
        try? data.write(to: Self.url(id), options: .atomic)
    }
}

// MARK: - The settings page

/// One game's settings, opened from its page or from its tile's menu.
struct TLAppSettingsView: View {
    let app: TLApp

    @ObservedObject private var store = TranslationLayerStore.shared
    @ObservedObject private var geode = GeodeSupport.shared
    @State private var settings: TLAppSettings
    @State private var name: String
    @State private var dataSize: String = "…"
    @State private var confirmReset = false
    @FocusState private var nameFocused: Bool

    init(app: TLApp) {
        self.app = app
        _settings = State(initialValue: TLAppSettings.load(app.id))
        _name = State(initialValue: app.label)
    }

    /// The game is loaded in this run of Husk, so what it keeps cannot be swapped from under it.
    private var inUse: Bool {
        guard let loaded = husk_native_loaded_apk().map({ String(cString: $0) }), let apk = app.apks.first else { return false }
        return loaded == apk
    }

    private var dataDirs: [URL] {
        let dir = TranslationLayer.root.appendingPathComponent(app.id, isDirectory: true)
        return ["unity-data", "cocos-data", "minecraft-data", "sdl-data", "ue4-data", "gta-data", "godot-data", "na-data"]
            .map { dir.appendingPathComponent($0, isDirectory: true) }
            .filter { FileManager.default.fileExists(atPath: $0.path) }
    }

    var body: some View {
        Form {
            Section {
                HStack(spacing: 14) {
                    GameIcon(app: app, size: 56)
                    VStack(alignment: .leading, spacing: 3) {
                        TextField("Name", text: $name)
                            .focused($nameFocused)
                            .submitLabel(.done)
                            .onSubmit { rename() }
                            .font(.system(size: 18, weight: .semibold))
                        Text(app.engineLabel).font(.subheadline).foregroundStyle(.secondary)
                    }
                }
                .padding(.vertical, 4)
            } footer: {
                Text("What the game is called in Husk. The game itself is not changed.")
            }

            Section {
                Picker("Orientation", selection: $settings.orientation) {
                    ForEach(TLAppSettings.Orientation.allCases) { Text($0.title).tag($0) }
                }
                Picker(selection: resolutionChoice) {
                    ForEach(GameDisplay.resolutions, id: \.self) { Text(GameDisplay.title($0)).tag($0) }
                } label: {
                    customLabel("Resolution", custom: resolutionIsCustom)
                }
                Picker(selection: scalingChoice) {
                    ForEach(GameScaling.allCases) { Text($0.title).tag($0) }
                } label: {
                    customLabel("Screen Scaling", custom: scalingIsCustom)
                }
                Picker(selection: frameRateChoice) {
                    ForEach(GameDisplay.frameRates, id: \.self) { Text(GameDisplay.frameRateTitle($0)).tag($0) }
                } label: {
                    customLabel("Frame Rate", custom: frameRateIsCustom)
                }
                if resolutionIsCustom || scalingIsCustom || frameRateIsCustom {
                    Button("Use Husk's Settings") {
                        settings.resolution = GameDisplay.followDefault
                        settings.scaling = GameDisplay.followDefault
                        settings.frameRate = GameDisplay.followDefault
                    }
                }
                Toggle("Full Screen", isOn: $settings.cleanView)
                Toggle("Keep the Screen On", isOn: $settings.keepAwake)
            } header: {
                Text("Display")
            } footer: {
                Text(displayFooter)
            }

            if app.packageName == GeodeSupport.gamePackage {
                Section {
                    Toggle("Geode", isOn: $settings.geode)
                    if settings.geode { geodeStatus }
                } header: {
                    Text("Mods")
                } footer: {
                    Text("Geode is the mod loader for Geometry Dash. With it on, Husk downloads Geode for this version of the "
                       + "game, and mods are found and installed in the game itself, from Geode's button on the main menu. "
                       + "Applies the next time the game starts.")
                }
            }

            Section {
                Picker("On-Screen Controller", selection: $settings.pad) {
                    ForEach(TLAppSettings.PadMode.allCases) { Text($0.title).tag($0) }
                }
                if settings.pad != .never {
                    VStack(alignment: .leading, spacing: 6) {
                        HStack {
                            Text("Opacity")
                            Spacer()
                            Text("\(Int((settings.padOpacity * 100).rounded()))%")
                                .font(.technical(14)).foregroundStyle(.secondary)
                        }
                        Slider(value: $settings.padOpacity, in: 0.3...1)
                    }
                    Toggle("Vibrate on a Press", isOn: $settings.haptics)
                }
            } header: {
                Text("Controller")
            } footer: {
                Text("Automatic offers the controller for games that cannot be played without one (Unreal Engine games), when no real "
                   + "controller is connected. Always offers it for any game that understands one. A paired controller is always "
                   + "used. In the game, tap with three fingers to show, hide or rearrange it.")
            }

            Section {
                LabeledContent("Saved by the Game", value: dataSize)
                Button(role: .destructive) { confirmReset = true } label: {
                    Text("Reset Game Data")
                }
                .disabled(inUse || dataDirs.isEmpty)
            } header: {
                Text("Data")
            } footer: {
                Text(inUse ? "The game is loaded in this session. Close Husk completely and open it again to reset its data."
                           : "Deletes what the game saved and downloaded here: saves, settings and caches. The game itself stays.")
            }
        }
        .huskForm()
        .navigationTitle("Game Settings")
        .navigationBarTitleDisplayMode(.inline)
        .onChange(of: settings) { $0.save(app.id) }
        .onChange(of: settings.geode) { on in
            if on { Task { await geode.prepare(app) } } else { geode.remove(app) }
        }
        .onChange(of: nameFocused) { focused in if !focused { rename() } }
        .onDisappear { rename() }
        .task { dataSize = await Self.measure(dataDirs) }
        .confirmationDialog("Reset the data of \(app.label)?", isPresented: $confirmReset, titleVisibility: .visible) {
            Button("Reset Game Data", role: .destructive) {
                for dir in dataDirs { try? FileManager.default.removeItem(at: dir) }
                HuskLog.log("tl", "reset the data of \(app.label)")
                Task { dataSize = await Self.measure(dataDirs) }
            }
            Button("Cancel", role: .cancel) { }
        } message: {
            Text("Saves, settings and caches the game made here are deleted.")
        }
    }

    // MARK: resolution and scaling: Husk's Settings unless this game says otherwise

    /// What the Display section's choices do. Built a line at a time: as one expression it is more than the type checker will take.
    private var displayFooter: String {
        let custom = resolutionIsCustom || scalingIsCustom || frameRateIsCustom
        var lines: [String] = []
        lines.append(GameDisplay.detail(GameDisplay.resolution(for: settings)))
        lines.append(GameDisplay.scaling(for: settings).detail)
        lines.append(GameDisplay.frameRateDetail(GameDisplay.frameRate(for: settings)))
        lines.append(custom ? "Custom is this game's own choice; the others follow Husk's Settings."
                            : "These follow Husk's Settings until you choose something else here.")
        lines.append("Full Screen draws the game around the camera too. These apply the next time the game starts; a game "
                     + "already running in this session needs Husk closed and opened again.")
        return lines.joined(separator: " ")
    }

    /// Whether this game has its own resolution or scaling rather than Husk's.
    private var resolutionIsCustom: Bool { GameDisplay.resolutions.contains(settings.resolution) }
    private var scalingIsCustom: Bool { GameScaling(rawValue: settings.scaling) != nil }
    private var frameRateIsCustom: Bool { GameDisplay.frameRates.contains(settings.frameRate) }

    /// The pickers show what the game will use. Choosing what Husk's Settings already say goes back to following them, so a
    /// later change there reaches this game too; choosing anything else makes it this game's own.
    private var resolutionChoice: Binding<String> {
        Binding(get: { GameDisplay.resolution(for: settings) },
                set: { settings.resolution = $0 == GameDisplay.savedResolution ? GameDisplay.followDefault : $0 })
    }
    private var scalingChoice: Binding<GameScaling> {
        Binding(get: { GameDisplay.scaling(for: settings) },
                set: { settings.scaling = $0 == GameDisplay.savedScaling ? GameDisplay.followDefault : $0.rawValue })
    }
    private var frameRateChoice: Binding<String> {
        Binding(get: { GameDisplay.frameRate(for: settings) },
                set: { settings.frameRate = $0 == GameDisplay.savedFrameRate ? GameDisplay.followDefault : $0 })
    }

    /// A row's name, with a tag when the game's choice differs from Husk's Settings.
    private func customLabel(_ title: String, custom: Bool) -> some View {
        HStack(spacing: 8) {
            Text(title)
            if custom { Tag(text: "Custom", tint: Theme.accent) }
        }
    }

    @ViewBuilder
    private var geodeStatus: some View {
        switch geode.current(app) {
        case .idle:
            Button("Download Geode") { Task { await geode.prepare(app) } }
        case .working(let what):
            HStack(spacing: 10) { ProgressView(); Text(what).foregroundStyle(.secondary) }
        case .ready(let version):
            LabeledContent("Geode", value: "v\(version), ready")
        case .failed(let why):
            VStack(alignment: .leading, spacing: 6) {
                Text(why).font(.footnote).foregroundStyle(.orange)
                Button("Try Again") { Task { await geode.prepare(app) } }
            }
        }
    }

    private func rename() {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, trimmed != app.label else { return }
        store.rename(app, to: trimmed)
    }

    private static func measure(_ dirs: [URL]) async -> String {
        await Task.detached(priority: .utility) {
            var bytes: Int64 = 0
            for dir in dirs {
                let walker = FileManager.default.enumerator(at: dir, includingPropertiesForKeys: [.fileSizeKey, .isRegularFileKey])
                while let url = walker?.nextObject() as? URL {
                    let values = try? url.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
                    if values?.isRegularFile == true { bytes += Int64(values?.fileSize ?? 0) }
                }
            }
            return bytes == 0 ? "Nothing" : ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
        }.value
    }
}
