// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// The graphics menu an Android bundle of an Unreal game can come with: both of Sifu's have one, in Java, with its strings in assets/avgraphics517.json (the
/// newer) or assets/graphics534.json (the older), by version code. No Java runs here, so the dialog cannot show. All it does, though, is send the engine console
/// commands, through GameActivity.nativeConsoleCommand, which works here too. So Husk has the same menu, opened from the toolbar over the game, in the bundle's own
/// words and languages. The two menus are the same but for two commands (tools/analysis/mod_menu_compare.py), so each bundle gets what its own menu sends.
///
/// Left out: its resolution and FPS counter, which Husk has already (the game's settings, the toolbar), and its ambient occlusion and reflections, which on
/// this renderer are compute shaders, which do not run here (OpenGL ES 3.0). An option never chosen is not sent, so the game keeps its own value.
///
/// On Android the bundle sends its settings at each launch, and always r.MaterialQualityLevel 2 with them, whatever is chosen. So does this.
enum ModGraphics {
    struct Choice {
        /// A key of the bundle's strings, or the text itself after a #.
        let label: String
        let commands: [String]
        /// What the older bundle's menu sends for it, where that differs.
        var olderCommands: [String]? = nil
    }

    struct Option: Identifiable {
        let id: String
        let advanced: Bool
        let choices: [Choice]
    }

    struct Preset {
        let label: String
        /// The choice for every option but the frame limit, which a preset leaves as it is.
        let values: [String: Int]
    }

    private static let levels = ["quality_low", "quality_medium", "quality_high"]
    private static let offOn = ["off", "on"]
    private static let offLowHigh = ["off", "quality_low", "quality_high"]

    /// One choice for each label; each console variable takes the value at that choice's place in its list.
    private static func option(_ id: String, advanced: Bool = false, _ labels: [String], _ variables: [(String, [String])], older: [(String, [String])]? = nil) -> Option {
        Option(id: id, advanced: advanced, choices: labels.indices.map { i in
            Choice(label: labels[i], commands: variables.map { "\($0.0) \($0.1[i])" }, olderCommands: older?.map { "\($0.0) \($0.1[i])" })
        })
    }

    /// As the bundle's Java has them (q.p517.p for the choices, q.p517.w.c for the commands).
    static let options: [Option] = [
        option("limit", ["#30 FPS", "#60 FPS", "#90 FPS", "#120 FPS", "unlimited"], [("t.MaxFPS", ["30", "60", "90", "120", "0"])]),
        option("aa", ["off", "#FXAA", "#TAA", "#MSAA 2×", "#MSAA 4×", "#MSAA 8×"], [
            ("r.DefaultFeature.AntiAliasing", ["0", "1", "2", "3", "3", "3"]),
            ("r.MobileMSAA", ["1", "1", "1", "2", "4", "8"]),
            ("r.PostProcessAAQuality", ["0", "2", "3", "0", "0", "0"]),
        ]),
        option("textures", levels, [("r.Streaming.MipBias", ["2.5", "1", "0"])]),
        option("filter", ["#1×", "#2×", "#4×", "#8×", "#16×"], [("r.MaxAnisotropy", ["1", "2", "4", "8", "16"])]),
        option("shadows", ["off", "quality_low", "quality_medium", "quality_high"], [("r.ShadowQuality", ["0", "2", "3", "5"])]),
        option("view", levels, [("r.ViewDistanceScale", ["0.55", "0.9", "1.2"])]),
        option("characters", levels, [("r.SkeletalMeshLODBias", ["2", "1", "0"])]),
        option("scenery", levels, [("r.StaticMeshLODDistanceScale", ["2.5", "2", "1"])]),
        option("detail", levels, [("r.DetailMode", ["0", "1", "2"])]),
        option("effects", levels, [("r.Tonemapper.Quality", ["0", "3", "5"]), ("r.LightFunctionQuality", ["0", "1", "1"])]),
        option("particles", levels, [
            ("r.EmitterSpawnRateScale", ["0.2", "0.65", "1"]),
            ("r.ParticleLODBias", ["2", "1", "0"]),
            ("fx.Niagara.QualityLevel", ["0", "1", "2"]),
        ]),
        option("vegetation", levels, [("foliage.DensityScale", ["0.25", "0.65", "1"]), ("grass.DensityScale", ["0.25", "0.65", "1"])]),
        option("bloom", offLowHigh, [("r.BloomQuality", ["0", "3", "5"])]),
        option("dof", offOn, [("r.DepthOfFieldQuality", ["0", "1"]), ("r.MobileHQGaussian", ["0", "1"])], older: [("r.DepthOfFieldQuality", ["0", "1"])]),
        option("film", offOn, [("r.Mobile.TonemapperFilm", ["0", "1"])]),
        Option(id: "pool", advanced: false, choices: [
            Choice(label: "auto", commands: ["r.Streaming.UseFixedPoolSize 0"]),
            Choice(label: "#256 MB", commands: ["r.Streaming.UseFixedPoolSize 1", "r.Streaming.PoolSize 256"]),
            Choice(label: "#384 MB", commands: ["r.Streaming.UseFixedPoolSize 1", "r.Streaming.PoolSize 384"]),
            Choice(label: "#512 MB", commands: ["r.Streaming.UseFixedPoolSize 1", "r.Streaming.PoolSize 512"]),
        ]),
        option("shadow_size", advanced: true, ["#256", "#512", "#1024", "#2048"], [("r.Shadow.MaxResolution", ["256", "512", "1024", "2048"])],
               older: [("r.Shadow.MaxResolution", ["256", "512", "1024", "2048"]), ("r.Shadow.MaxCSMResolution", ["256", "512", "1024", "2048"])]),
        option("shadow_distance", advanced: true, levels, [("r.Shadow.DistanceScale", ["0.4", "0.7", "1"])]),
        option("cascades", advanced: true, ["#1", "#2"], [("r.Shadow.CSM.MaxCascades", ["1", "2"])]),
        option("vegetation_distance", advanced: true, levels, [("foliage.LODDistanceScale", ["0.25", "0.65", "1"]), ("grass.CullDistanceScale", ["0.5", "0.75", "1"])]),
        option("vsync", advanced: true, offOn, [("r.VSync", ["0", "1"])]),
        option("decal_time", advanced: true, levels, [("r.Decal.FadeDurationScale", ["0.5", "1", "2"])]),
        option("decal_distance", advanced: true, levels, [("r.Decal.FadeScreenSizeMult", ["2", "1", "0.5"])]),
        option("shafts", advanced: true, offOn, [("r.LightShaftQuality", ["0", "1"])]),
        option("refraction", advanced: true, offLowHigh, [("r.RefractionQuality", ["0", "1", "2"])]),
        option("chromatic", advanced: true, offOn, [("r.SceneColorFringeQuality", ["0", "1"])]),
        option("latency", advanced: true, offOn, [("r.OneFrameThreadLag", ["1", "0"])]),
        option("fog", advanced: true, offOn, [("r.Fog", ["0", "1"])]),
    ]

    private static func preset(_ label: String, _ raised: [String: Int]) -> Preset {
        var values: [String: Int] = [:]
        for option in options where option.id != "limit" { values[option.id] = raised[option.id] ?? 0 }
        return Preset(label: label, values: values)
    }

    /// As q.p517.p.d sets them.
    static let presets: [Preset] = [
        preset("minimum", [:]),
        preset("low", ["aa": 1, "textures": 1, "filter": 1, "shadows": 1, "shadow_size": 1]),
        preset("preset_medium", ["aa": 3, "textures": 1, "filter": 2, "shadows": 2, "shadow_size": 2, "shadow_distance": 1, "cascades": 1,
                                 "view": 1, "characters": 1, "scenery": 1, "detail": 1, "effects": 1, "particles": 1, "vegetation": 1, "vegetation_distance": 1,
                                 "bloom": 1, "film": 1, "decal_time": 1, "decal_distance": 1, "shafts": 1, "refraction": 1, "fog": 1]),
        preset("preset_high", ["aa": 4, "textures": 2, "filter": 3, "shadows": 3, "shadow_size": 3, "shadow_distance": 2, "cascades": 1,
                               "view": 2, "characters": 2, "scenery": 2, "detail": 2, "effects": 2, "particles": 2, "vegetation": 2, "vegetation_distance": 2,
                               "bloom": 2, "dof": 1, "film": 1, "decal_time": 2, "decal_distance": 2, "shafts": 1, "refraction": 2, "chromatic": 1, "fog": 1]),
    ]

    /// What the choices come to, as console commands, for the menu of the older bundle or the newer.
    static func commands(_ selection: [String: Int], older: Bool) -> [String] {
        var out: [String] = []
        for option in options {
            guard let chosen = selection[option.id], option.choices.indices.contains(chosen) else { continue }
            let choice = option.choices[chosen]
            out += older ? (choice.olderCommands ?? choice.commands) : choice.commands
        }
        return out + ["r.MaterialQualityLevel 2"]
    }

    static func send(_ selection: [String: Int], older: Bool) {
        for command in commands(selection, older: older) { husk_native_console_command(command) }
    }

    /// The bundle's words for its menu, in the phone's language where it has it.
    struct Strings {
        let text: [String: String]
        /// That these are the older bundle's (graphics534.json), whose menu sends a little differently.
        let older: Bool

        func callAsFunction(_ key: String) -> String {
            key.hasPrefix("#") ? String(key.dropFirst()) : (text[key] ?? key)
        }
    }

    /// The menu's strings, when the game's APK has a menu. They are named by the APK's version code.
    static func strings(apk: String) -> Strings? {
        guard let manifest = TranslationLayerStore.entry(apk, "AndroidManifest.xml", limit: 8 << 20),
              let version = ApkMetadata.versionCode(manifest) else { return nil }
        for (name, older) in [("assets/avgraphics\(version).json", false), ("assets/graphics\(version).json", true)] {
            guard let data = TranslationLayerStore.entry(apk, name, limit: 4 << 20),
                  let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
                  let keys = json["keys"] as? [String],
                  let locales = json["locales"] as? [String: [String]] else { continue }
            // The longest of its languages that the phone's starts with (zh-Hans for zh-Hans-CN), or else one in the same language (pt-BR for pt-PT).
            let preferred = (Locale.preferredLanguages.first ?? "en").lowercased()
            let exact = locales.keys.filter { preferred.hasPrefix($0.lowercased()) }.max { $0.count < $1.count }
            let same = locales.keys.filter { $0.lowercased().hasPrefix(String(preferred.prefix(2))) }.min()
            guard let words = locales[exact ?? same ?? "en"] ?? locales["en"], words.count == keys.count else { continue }
            return Strings(text: Dictionary(zip(keys, words), uniquingKeysWith: { first, _ in first }), older: older)
        }
        return nil
    }
}

/// The menu itself, over the game. Nothing is sent until Apply.
struct ModGraphicsPanel: View {
    let strings: ModGraphics.Strings
    let selected: [String: Int]
    let onApply: ([String: Int]) -> Void
    let onClose: () -> Void
    @State private var draft: [String: Int] = [:]
    @State private var advanced = false

    var body: some View {
        VStack(spacing: 10) {
            HStack {
                Text(strings("graphics"))
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(.white)
                Spacer()
                Button(action: onClose) {
                    Image(systemName: "xmark")
                        .font(.system(size: 11, weight: .bold))
                        .foregroundStyle(.white)
                        .frame(width: 26, height: 26)
                        .background(Color.white.opacity(0.12), in: Circle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(strings("cancel"))
            }

            Picker("", selection: $advanced) {
                Text(strings("basic")).tag(false)
                Text(strings("advanced")).tag(true)
            }
            .pickerStyle(.segmented)

            ScrollView {
                VStack(spacing: 10) {
                    if !advanced { presetRow }
                    ForEach(ModGraphics.options.filter { $0.advanced == advanced }) { option in
                        row(option)
                    }
                    Text(strings(advanced ? "advanced_hint" : "hint"))
                        .font(.system(size: 11))
                        .foregroundStyle(.white.opacity(0.6))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.top, 4)
                }
            }

            HStack(spacing: 10) {
                Button(strings("cancel"), action: onClose)
                    .buttonStyle(.bordered)
                    .tint(.white)
                Button(strings("apply")) { onApply(draft) }
                    .buttonStyle(.borderedProminent)
            }
        }
        .padding(12)
        .background(Color.black.opacity(0.85), in: RoundedRectangle(cornerRadius: 16, style: .continuous))
        .onAppear { draft = selected }
    }

    private func matches(_ preset: ModGraphics.Preset) -> Bool {
        preset.values.allSatisfy { draft[$0.key] == $0.value }
    }

    private func current(_ option: ModGraphics.Option) -> String {
        guard let chosen = draft[option.id], option.choices.indices.contains(chosen) else { return strings("auto") }
        return strings(option.choices[chosen].label)
    }

    private var presetRow: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(strings("preset"))
                .font(.system(size: 13, weight: .medium))
                .foregroundStyle(.white)
            HStack(spacing: 6) {
                ForEach(ModGraphics.presets.indices, id: \.self) { i in
                    let preset = ModGraphics.presets[i]
                    Button {
                        for (id, value) in preset.values { draft[id] = value }
                    } label: {
                        Text(strings(preset.label))
                            .font(.system(size: 12, weight: .medium))
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered)
                    .tint(matches(preset) ? Color.accentColor : Color.white)
                }
            }
        }
    }

    private func row(_ option: ModGraphics.Option) -> some View {
        HStack {
            Text(strings(option.id))
                .font(.system(size: 13))
                .foregroundStyle(.white)
            Spacer(minLength: 8)
            Menu {
                Button(strings("auto")) { draft[option.id] = nil }
                ForEach(option.choices.indices, id: \.self) { i in
                    Button(strings(option.choices[i].label)) { draft[option.id] = i }
                }
            } label: {
                Text(current(option))
                    .font(.system(size: 13, weight: .medium))
            }
            .tint(.white)
        }
    }
}
