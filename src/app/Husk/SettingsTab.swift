// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// Settings, as the iOS Settings app lays them out: a card for the app up top, then groups of rows. What belongs to games,
/// what belongs to the app, and what the thing is.
struct SettingsTab: View {
    @ObservedObject private var jit = JITCoordinator.shared
    @AppStorage(PerfOverlay.enabledKey) private var perfOn = false
    @AppStorage(PerfOverlay.positionKey) private var perfPosition: PerfOverlay.Position = .topLeading
    @AppStorage(PerfOverlay.detailedKey) private var perfDetailed = true
    @AppStorage(TranslationLayer.devInfoKey) private var devInfo = false
    @State private var showLogs = false

    var body: some View {
        NavigationStack {
            List {
                Section {
                    NavigationLink { AboutSettings() } label: { appCard }
                }

                Section {
                    NavigationLink { JITSettings() } label: {
                        row("bolt.fill", .yellow, "JIT", detail: Launcher.jitOn ? "On" : "Off")
                    }
                } footer: {
                    Text("Games need JIT to run. StikJIT, built into Husk, turns it on without a computer.")
                }

                Section {
                    Toggle(isOn: $perfOn) { row("speedometer", .orange, "Performance Overlay") }
                    if perfOn {
                        Picker(selection: $perfPosition) {
                            ForEach(PerfOverlay.Position.allCases) { Text($0.title).tag($0) }
                        } label: { row("rectangle.inset.topleft.filled", .gray, "Position") }
                        Toggle(isOn: $perfDetailed) { row("list.bullet.rectangle", .gray, "Frame Time & Memory") }
                    }
                } header: {
                    Text("In Game")
                } footer: {
                    Text(perfOn
                         ? "Shows the frame rate over every game\(perfDetailed ? ", with the time a frame takes, the memory Husk uses and a warning when the iPhone runs hot" : "")."
                           + " Tap the top of the screen in a game to bring up its controls."
                         : "Tap the top of the screen in a game to bring up its controls: close the game, the on-screen "
                           + "controller, this overlay and the game's log.")
                }

                Section {
                    NavigationLink { AppearanceSettings() } label: {
                        row("paintbrush.fill", .pink, "Appearance")
                    }
                }

                Section {
                    Toggle(isOn: $devInfo) { row("hammer.fill", .indigo, "Developer Info") }
                    if devInfo {
                        NavigationLink { TLChecksView() } label: { row("stethoscope", .teal, "Device Checks") }
                    }
                    Button { showLogs = true } label: { row("terminal.fill", .gray, "Console") }
                        .foregroundStyle(Theme.text)
                } header: {
                    Text("Troubleshooting")
                } footer: {
                    Text("The console is Husk's live log. In a game, the log button in its toolbar shows that game's own run log. "
                       + "Both are what to share when something goes wrong.")
                }
            }
            .listStyle(.insetGrouped)
            .navigationTitle("Settings")
            .sheet(isPresented: $showLogs) { LogView() }
        }
    }

    private var appCard: some View {
        HStack(spacing: 14) {
            HuskMark(size: 58)
            VStack(alignment: .leading, spacing: 3) {
                Text("Husk").font(.display(22))
                Text("Version \(Bundle.main.version) · \(Bundle.main.commit)")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 6)
    }

    private func row(_ icon: String, _ tint: Color, _ title: String, detail: String? = nil) -> some View {
        HStack(spacing: 12) {
            SettingsIcon(systemImage: icon, tint: tint)
            Text(title)
            if let detail {
                Spacer()
                Text(detail).foregroundStyle(.secondary)
            }
        }
    }
}

/// A Form, as the system draws one.
private struct HuskForm: ViewModifier {
    func body(content: Content) -> some View {
        content.navigationBarTitleDisplayMode(.inline)
    }
}

extension View {
    func huskForm() -> some View { modifier(HuskForm()) }
}

// MARK: - JIT

struct JITSettings: View {
    @ObservedObject private var jit = JITCoordinator.shared
    @AppStorage(JITCoordinator.autoEnableKey) private var autoJIT = false
    @State private var keepAttached = JITBootstrap.keepDebuggerAttached

    private var pairingLabel: String {
        switch jit.pairingSource {
        case .onDevice: return "Paired on this device"
        case .imported: return "File imported"
        case nil: return "Not set up"
        }
    }

    var body: some View {
        Form {
            Section {
                JITCard()
            }
            .listRowInsets(EdgeInsets())
            .listRowBackground(Color.clear)

            Section {
                Toggle("Turn On JIT at Launch", isOn: $autoJIT)
                    .disabled(!HuskBuiltInJIT.isAvailable)
            } footer: {
                Text(HuskBuiltInJIT.isAvailable
                     ? "Each time Husk opens without JIT, it asks the built-in StikJIT to turn it on, so games are ready "
                       + "without a tap. Needs StikJIT set up once (paired) first."
                     : "Needs the built-in StikJIT, which is available on iOS 26 and later.")
            }

            Section {
                Picker("Method", selection: $jit.method) {
                    ForEach(JITMethod.allCases) { Text($0.title).tag($0) }
                }
                LabeledContent("Built-in Pairing", value: pairingLabel)
                LabeledContent("StikDebug", value: JITBootstrap.isStikDebugInstalled ? "Installed" : "Not found")
                LabeledContent("TrollStore", value: JITBootstrap.isTrollStoreInstalled ? "Installed" : "Not found")
                LabeledContent("Installed with TrollStore", value: JITBootstrap.isInstalledWithTrollStore ? "Yes" : "No")
                LabeledContent("Jailbreak", value: JITBootstrap.debuggedAtLaunch ? "JIT allowed for apps"
                               : JITBootstrap.isJailbroken ? "Found; Allow JIT in Apps is off" : "Not found")
                Button {
                    jit.showSetup = true
                } label: {
                    Label("Set Up JIT", systemImage: "wand.and.stars")
                }
            } header: {
                Text("Method")
            } footer: {
                Text(jit.method == .automatic
                     ? jit.automaticDescription + " Built-in StikJIT needs iOS 26, LocalDevVPN, and a "
                       + "pairing file, which Husk can make itself on iOS 27."
                     : HuskBuiltInJIT.unavailableReason ?? "Built-in StikJIT needs LocalDevVPN and a pairing "
                       + "file, which Husk can make itself on iOS 27.")
            }

            Section {
                LabeledContent("Debugger", value: JITBootstrap.isDebuggerAttached ? "Attached" : "Not attached")
                LabeledContent("Executable Memory", value: JITBootstrap.isLive ? "Granted" : "Not claimed")
                // The two routes, named separately. Either one is enough, and when someone reports "JIT does not work"
                // these two rows are the whole diagnosis.
                LabeledContent("Trap Servicer", value: JITBootstrap.prewarmed ? "Answering" : "Not answering")
                // Cached answer only: running the probe from a view body could freeze the app (see JITBootstrap.mapJITWorks).
                LabeledContent("MAP_JIT", value: JITBootstrap.deviceEnforcesTXM ? "Not used (TXM)"
                               : JITBootstrap.mapJITResult.map { $0 ? "Executes" : "Refused" } ?? "Not tested")
                LabeledContent("Debugger After Setup", value: JITBootstrap.detached ? "Detached" : "Attached")
                if let why = JITBootstrap.lastFailure {
                    Text(why).font(.caption).foregroundStyle(.orange)
                }
                if !JITBootstrap.isDebuggerAttached {
                    Button {
                        jit.enable()
                    } label: {
                        Label("Enable JIT with \(jit.resolvedMethod.title)", systemImage: "bolt.fill")
                    }
                    .disabled(jit.busy)
                    Button {
                        _ = JITBootstrap.requestTrollStoreAttach()
                    } label: {
                        Label("Enable JIT with TrollStore", systemImage: "sparkles")
                    }
                }
            } header: {
                Text("Status")
            } footer: {
                Text("Games need memory they can write and then execute, which on iOS takes an attached debugger. There are "
                   + "two ways to get it: a debugger that services trap requests, or a MAP_JIT mapping, which the kernel allows "
                   + "any debugged process. Either one is enough — which is available depends on the device and the iOS "
                   + "version, so Husk tests both rather than assuming.")
            }

            Section {
                Toggle("Keep Debugger Attached", isOn: $keepAttached)
                    .onChange(of: keepAttached) { v in JITBootstrap.keepDebuggerAttached = v }
            } footer: {
                Text("Off by default. Husk detaches StikDebug as soon as the JIT region is held, because a debugger that iOS has "
                   + "suspended stops the whole app the next time it is needed. Turn this on only to collect StikDebug's own logs.")
            }
        }
        .huskForm()
        .navigationTitle("JIT")
    }
}

// MARK: - Appearance

struct AppearanceSettings: View {
    @State private var appIcon = HuskAppIcon.current
    @AppStorage(Theme.Appearance.key) private var appearance = Theme.Appearance.system
    @ObservedObject private var theme = AppTheme.shared
    @Environment(\.colorScheme) private var scheme
    @State private var custom: Color = AppTheme.shared.accentColor

    private let iconColumns = [GridItem(.adaptive(minimum: 92), spacing: 14)]

    var body: some View {
        Form {
            Section {
                Picker("Appearance", selection: $appearance) {
                    ForEach(Theme.Appearance.allCases) { Text($0.title).tag($0) }
                }
                .pickerStyle(.segmented)
                .onChange(of: appearance) { v in
                    HuskLog.log("ui", "appearance: \(v.rawValue)")
                }
            } header: {
                Text("Theme")
            } footer: {
                Text("System follows the phone: light by day, dark by night. A game's toolbar stays dark either way, so it "
                   + "reads over any game.")
            }

            Section {
                LazyVGrid(columns: Array(repeating: GridItem(.flexible()), count: 5), spacing: 14) {
                    ForEach(AppTheme.presets) { preset in
                        Button {
                            theme.accentColor = preset.color
                            custom = preset.color
                        } label: {
                            swatch(preset.color, selected: preset.color.themeHex == theme.accentColor.themeHex)
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(preset.name)
                    }
                }
                .padding(.vertical, 6)

                ColorPicker("Custom Colour", selection: $custom, supportsOpacity: false)
                    .onChange(of: custom) { theme.accentColor = $0 }
            } header: {
                Text("Accent Colour")
            } footer: {
                Text("Tints buttons, switches and selected states throughout the app.")
            }

            Section {
                LazyVGrid(columns: iconColumns, spacing: 14) {
                    ForEach(HuskAppIcon.allCases) { icon in
                        Button {
                            appIcon = icon
                            HuskAppIcon.apply(icon)
                        } label: {
                            VStack(spacing: 8) {
                                if let art = icon.preview(dark: scheme == .dark) {
                                    Image(uiImage: art)
                                        .resizable().scaledToFit()
                                        .frame(width: 60, height: 60)
                                        .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
                                        .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous)
                                            .strokeBorder(appIcon == icon ? Color.accentColor : .clear, lineWidth: 3))
                                }
                                Text(icon.title)
                                    .font(.caption)
                                    .foregroundStyle(appIcon == icon ? Color.accentColor : .primary)
                                    .lineLimit(1)
                            }
                            .frame(maxWidth: .infinity)
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(.vertical, 6)
            } header: {
                Text("App Icon")
            } footer: {
                Text("Automatic follows the system appearance — light, dark and tinted. The others pin "
                   + "one look. iOS shows its own confirmation after a change; that alert cannot be turned off.")
            }
        }
        .navigationTitle("Appearance")
        .navigationBarTitleDisplayMode(.inline)
    }

    private func swatch(_ color: Color, selected: Bool) -> some View {
        Circle()
            .fill(color)
            .frame(width: 34, height: 34)
            .overlay(Circle().strokeBorder(Color.primary.opacity(0.9), lineWidth: selected ? 2 : 0).padding(-4))
            .overlay {
                if selected {
                    Image(systemName: "checkmark")
                        .font(.caption.bold())
                        .foregroundStyle(color.isLight ? .black : .white)
                }
            }
            .frame(maxWidth: .infinity)
    }
}

// MARK: - About

struct AboutSettings: View {
    @ObservedObject private var store = TranslationLayerStore.shared

    var body: some View {
        List {
            Section {
                VStack(spacing: 10) {
                    HuskMark(size: 88)
                        .shadow(color: Theme.shadow, radius: 14, y: 6)
                    Text("Husk").font(.display(28))
                    Text("Android games, running natively on iPhone.")
                        .font(.subheadline).foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 12)
                .listRowBackground(Color.clear)
            }

            Section {
                LabeledContent("Version", value: Bundle.main.version)
                LabeledContent("Build", value: Bundle.main.commit)
                LabeledContent("Games", value: "\(store.apps.count)")
            }

            Section {
                Text("Husk runs an Android game's own 64-bit code directly on the iPhone's processor, and stands in for "
                   + "Android around it: the C library, the Java calls the game makes, OpenGL ES through ANGLE, Vulkan "
                   + "through MoltenVK, sound, touch and controllers. Nothing is emulated and no Android boots, so a game "
                   + "starts in seconds.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            } header: {
                Text("How It Works")
            }

            Section {
                Text("Husk is free software under the GNU General Public License, version 2 or later. The licences of "
                   + "the libraries it includes are in the app's Resources.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            } header: {
                Text("Licence")
            }
        }
        .navigationTitle("About")
        .navigationBarTitleDisplayMode(.inline)
    }
}

extension Bundle {
    var version: String {
        (infoDictionary?["CFBundleShortVersionString"] as? String) ?? "?"
    }
    var commit: String {
        (infoDictionary?["HuskBuildCommit"] as? String) ?? "?"
    }
}
