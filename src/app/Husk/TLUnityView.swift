// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit
import QuartzCore
import AVFoundation

/// The engines the native runtime drives. Both draw into a CAMetalLayer through ANGLE, and both are one game per
/// process: an engine cannot be unloaded once it has started.
enum TLNativeEngine {
    case unity     // Subway Surfers and other Unity games: portrait, driven by UnityPlayer's own thread
    case cocos     // Geometry Dash and other cocos2d-x games: landscape, driven by a GL thread of our own
    case minecraft // Minecraft and other GameActivity games: landscape, multi-touch, the game runs its own threads
    case sdl       // Beach Buggy Racing 2 and other SDL3 games: landscape, multi-touch, the game runs its own threads
    case gta       // GTA San Andreas (Rockstar): landscape, touch and controllers, plain OpenGL ES
    case ue4       // Minecraft Dungeons and other Unreal Engine 4 games: landscape, touch and controllers, Vulkan on MoltenVK
    case godot     // A Godot 3 or 4 game: the manifest says which way up, OpenGL ES through ANGLE, multi-touch
    case nativeactivity // A game that is a NativeActivity library of its own (Open Golf): the manifest says which way up, OpenGL ES through ANGLE
}

/// A Unity game's screen: one CAMetalLayer that the game's own GL (ANGLE over Metal) presents into.
///
/// Nothing is copied or composed here. The runtime hands the layer to EGL as the game's window; the
/// game draws and presents on its own thread. This view's jobs are the layer's size, the pause that
/// goes with leaving the screen, and turning touches into the pixel coordinates Android reports.
final class TLUnityUIView: UIView, UIKeyInput, UIGestureRecognizerDelegate {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    /// The cocos2d-x or SDL game on screen, which the game's keyboard requests (they arrive on its own thread) are routed to.
    nonisolated(unsafe) static weak var cocosView: TLUnityUIView?

    private let apk: String
    /// The app's other APKs -- splits, an asset pack -- which an SDL game's libraries and data may be in.
    private let extraApks: [String]
    private let dataDir: String
    private let engine: TLNativeEngine
    /// A portrait game is told its size when the screen is taller than wide, as a landscape one is when it is wider.
    private let portrait: Bool
    private var launched = false
    /// Active touches by UITouch identity, each given a small stable id like Android's pointer ids.
    private var pointers: [ObjectIdentifier: Int32] = [:]

    /// Called when three fingers tap at once: another way to bring the game's toolbar up or put it away.
    var onThreeFingerTap: (() -> Void)?
    /// Called when one finger taps near the top edge: the way to bring the game's toolbar up.
    var onTopTap: (() -> Void)?
    /// How far down from the top edge a tap counts as one at the top, in points.
    var topZone: CGFloat = 48
    private var topTap: UITapGestureRecognizer?

    init(apk: String, extraApks: [String] = [], dataDir: String, engine: TLNativeEngine, portrait: Bool = false, scale: CGFloat = 2) {
        self.apk = apk
        self.portrait = portrait
        self.dataDir = dataDir
        self.engine = engine
        self.extraApks = extraApks
        super.init(frame: .zero)
        backgroundColor = .black
        isMultipleTouchEnabled = true
        // Two pixels per point unless the game's settings say otherwise: sharp enough, and a third of the pixels a 3x phone would
        // ask the game for -- a 3D game is limited by fill rate, and ANGLE's translation costs on top.
        contentScaleFactor = scale
        if let metal = layer as? CAMetalLayer {
            metal.pixelFormat = .bgra8Unorm
            metal.framebufferOnly = true
            metal.contentsScale = scale
            metal.isOpaque = true
        }
        let threeFingers = UITapGestureRecognizer(target: self, action: #selector(threeFingerTapped))
        threeFingers.numberOfTouchesRequired = 3
        threeFingers.cancelsTouchesInView = false
        threeFingers.delaysTouchesBegan = false
        threeFingers.delaysTouchesEnded = false
        addGestureRecognizer(threeFingers)

        // A tap near the top edge brings the toolbar down. It watches without taking anything: the game still gets the
        // touch, exactly as it would with no recognizer there.
        let top = UITapGestureRecognizer(target: self, action: #selector(topTapped))
        top.numberOfTouchesRequired = 1
        top.cancelsTouchesInView = false
        top.delaysTouchesBegan = false
        top.delaysTouchesEnded = false
        top.delegate = self
        addGestureRecognizer(top)
        topTap = top
        if engine == .cocos {
            TLUnityUIView.cocosView = self
            TLUnityUIView.installKeyboardHandler()
        } else if engine == .sdl {
            TLUnityUIView.cocosView = self
            TLUnityUIView.installSDLKeyboardHandler()
        } else if engine == .minecraft {
            TLUnityUIView.cocosView = self
            TLUnityUIView.installGameActivityKeyboardHandler()
        }
        // The GPU is not the app's while it is in the background: stop drawing, and carry on when it returns.
        NotificationCenter.default.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { _ in
            husk_unity_set_paused(true)
        }
        NotificationCenter.default.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            if self?.window != nil { husk_unity_set_paused(false) }
        }
    }

    @objc private func threeFingerTapped() { onThreeFingerTap?() }
    @objc private func topTapped() { onTopTap?() }

    func gestureRecognizer(_ g: UIGestureRecognizer, shouldReceive touch: UITouch) -> Bool {
        guard g === topTap else { return true }
        return touch.location(in: self).y <= topZone
    }

    func gestureRecognizer(_ g: UIGestureRecognizer,
                           shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool { true }

    deinit { NotificationCenter.default.removeObserver(self) }

    required init?(coder: NSCoder) { fatalError("not used") }

    override func layoutSubviews() {
        super.layoutSubviews()
        guard bounds.width > 0, bounds.height > 0 else { return }
        let w = Int((bounds.width * contentScaleFactor).rounded())
        let h = Int((bounds.height * contentScaleFactor).rounded())
        // An Unreal game draws with MoltenVK, which sizes this layer itself to the swapchain it made; sizing it back here on every layout would leave the layer and the
        // swapchain disagreeing from then on.
        if !(engine == .ue4 && launched) { (layer as? CAMetalLayer)?.drawableSize = CGSize(width: w, height: h) }
        // A landscape game is told its size once, when it starts, so it must not start while the screen is still
        // turning: wait for a surface that is wider than it is tall.
        // Unity games too: Fruit Ninja is a landscape one. If the screen has not turned after two seconds it is not going to,
        // and the game starts as it is rather than not at all.
        if firstLayout == nil, window != nil {
            firstLayout = Date()
            DispatchQueue.main.asyncAfter(deadline: .now() + 2.1) { [weak self] in self?.setNeedsLayout() }
        }
        let waited = firstLayout.map { Date().timeIntervalSince($0) > 2 } ?? false
        let ready = (portrait ? h > w : w > h) || waited
        if !launched, window != nil, ready { launch(width: w, height: h) }
    }

    private var firstLayout: Date?

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil {
            husk_unity_set_paused(false)
        } else {
            husk_unity_set_paused(true)
            if isFirstResponder { resignFirstResponder() }
        }
        setNeedsLayout()
    }

    private func launch(width: Int, height: Int) {
        launched = true
        let angle = (Bundle.main.privateFrameworksPath ?? "") + "/libANGLE-shared.dylib"
        let ca = Bundle.main.path(forResource: "cacert", ofType: "pem") ?? ""
        try? FileManager.default.createDirectory(atPath: dataDir, withIntermediateDirectories: true)
        let layerPtr = Unmanaged.passUnretained(layer).toOpaque()
        if husk_unity_state() != Int32(HUSK_UNITY_IDLE) {
            // Already started this run: the engine cannot be loaded twice, so just show it again.
            HuskLog.log("tl", "unity: already started; resuming")
            return
        }
        // The game plays through the silent switch, like the guest's own audio, and mixes with other audio. Unity games too:
        // they used to be left out, from before they had sound, and played through a session that was never set up.
        do {
            let session = AVAudioSession.sharedInstance()
            try? session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
            try? session.setActive(true)
        }
        HuskLog.log("tl", "native: launching \(apk) at \(width)x\(height) (\(engine == .cocos ? "cocos2d-x" : engine == .minecraft ? "gameactivity" : engine == .sdl ? "sdl" : engine == .ue4 ? "ue4" : engine == .gta ? "gta" : engine == .godot ? "godot" : engine == .nativeactivity ? "nativeactivity" : "unity"))")
        // Splits and the asset pack are part of the app, whatever its engine; the game's libraries and data may be in any of
        // them (a Google Play install keeps a Unity game's libraries in one split and its data in an asset pack).
        for extra in extraApks.prefix(3) { husk_native_add_package(extra) }
        let started: Bool
        switch engine {
        case .sdl:
            // The notch and the rounded corners, in the surface's pixels: the game keeps its controls out of them.
            if let inset = window?.safeAreaInsets {
                let k = contentScaleFactor
                husk_sdl_set_safe_insets(Int32(inset.left * k), Int32(inset.top * k), Int32(inset.right * k), Int32(inset.bottom * k))
            }
            started = husk_sdl_launch(apk, dataDir, layerPtr, Int32(width), Int32(height), angle, ca)
        case .gta: started = husk_gta_launch(apk, dataDir, layerPtr, Int32(width), Int32(height), angle, ca)
        case .godot: started = husk_godot_launch(apk, dataDir, layerPtr, Int32(width), Int32(height), angle, ca)
        case .nativeactivity: started = husk_ue4_launch(apk, dataDir, layerPtr, Int32(width), Int32(height), angle, ca)       // the NativeActivity driver; with no Unreal in the APK it runs the plain game
        case .ue4:
            // Unreal draws with Vulkan, which on this device is MoltenVK, a framework of the app's own.
            if let fw = Bundle.main.privateFrameworksPath { husk_ue4_set_vulkan(fw + "/MoltenVK.framework/MoltenVK") }
            started = husk_ue4_launch(apk, dataDir, layerPtr, Int32(width), Int32(height), angle, ca)
        case .cocos:
            // Geode, when it is on for this game and downloaded: loaded into the game after its own libraries.
            let geode = GeodeSupport.files(appDir: (dataDir as NSString).deletingLastPathComponent)
            husk_cocos_set_geode(geode?.zip, geode?.launcher)
            if geode != nil { HuskLog.log("geode", "loading Geode into the game") }
            started = husk_cocos_launch(apk, dataDir, layerPtr, Int32(width), Int32(height), angle, ca)
        case .minecraft: started = husk_gameactivity_launch(apk, dataDir, layerPtr, Int32(width), Int32(height), angle, ca)
        case .unity: started = husk_unity_launch(apk, dataDir, layerPtr, Int32(width), Int32(height), angle, ca)
        }
        if !started { HuskLog.log("tl", "native: launch refused") }
    }

    // MARK: keyboard (cocos2d-x and SDL games)

    /// A game asks for the keyboard when its text field is tapped. The keyboard belongs to this view; what it types goes
    /// to the game, and a strip above the keyboard shows the text, because in landscape the keyboard covers the game's field.
    override var canBecomeFirstResponder: Bool { engine == .cocos || engine == .sdl || engine == .minecraft }
    var hasText: Bool { true }
    var autocorrectionType: UITextAutocorrectionType = .no
    var autocapitalizationType: UITextAutocapitalizationType = .none
    var spellCheckingType: UITextSpellCheckingType = .no
    var smartQuotesType: UITextSmartQuotesType = .no
    var smartDashesType: UITextSmartDashesType = .no
    var smartInsertDeleteType: UITextSmartInsertDeleteType = .no
    var keyboardType: UIKeyboardType = .default
    var keyboardAppearance: UIKeyboardAppearance = .dark
    var returnKeyType: UIReturnKeyType = .done

    private var typed = ""
    private lazy var typedLabel: UILabel = {
        let l = UILabel()
        l.font = .systemFont(ofSize: 17, weight: .medium)
        l.textColor = .white
        l.lineBreakMode = .byTruncatingHead
        return l
    }()
    private lazy var keyboardBar: UIView = {
        let bar = UIView(frame: CGRect(x: 0, y: 0, width: 100, height: 44))
        bar.backgroundColor = UIColor(white: 0.12, alpha: 1)
        bar.autoresizingMask = [.flexibleWidth]
        typedLabel.frame = CGRect(x: 16, y: 0, width: 100, height: 44)
        typedLabel.autoresizingMask = [.flexibleWidth]
        bar.addSubview(typedLabel)
        let done = UIButton(type: .system)
        done.setTitle("Done", for: .normal)
        done.titleLabel?.font = .systemFont(ofSize: 17, weight: .semibold)
        done.frame = CGRect(x: 100, y: 0, width: 80, height: 44)
        done.autoresizingMask = [.flexibleLeftMargin]
        done.addAction(UIAction { [weak self] _ in self?.finishTyping() }, for: .touchUpInside)
        bar.addSubview(done)
        return bar
    }()
    override var inputAccessoryView: UIView? { engine == .cocos || engine == .sdl || engine == .minecraft ? keyboardBar : nil }

    func insertText(_ text: String) {
        if text == "\n" { finishTyping(); return }
        typed += text
        typedLabel.text = typed
        if engine == .sdl { husk_sdl_commit_text(text) } else if engine == .minecraft { husk_ga_insert_text(text) } else { husk_cocos_insert_text(text) }
    }

    func deleteBackward() {
        if !typed.isEmpty { typed.removeLast() }
        typedLabel.text = typed
        if engine == .sdl { husk_sdl_key(67, 1); husk_sdl_key(67, 0) }   // KEYCODE_DEL
        else if engine == .minecraft { husk_ga_delete_backward() }
        else { husk_cocos_delete_backward() }
    }

    private func setTyped(_ text: String) { typed = text; typedLabel.text = text }

    /// Return, or the Done button: what Android's "done" action does -- the game gets a newline, and the keyboard goes.
    private func finishTyping() {
        if engine == .sdl { husk_sdl_key(66, 1); husk_sdl_key(66, 0) }   // KEYCODE_ENTER
        else if engine == .minecraft { husk_ga_editor_action() }        // the field's own action: send the chat, name the world
        else { husk_cocos_insert_text("\n") }
        resignFirstResponder()
    }

    /// The game's own requests, from its GL thread: 0 toggles, 1 shows, 2 hides.
    static func installKeyboardHandler() {
        // A link in the game (terms of use, the social buttons) opens in the browser.
        husk_cocos_set_open_url_handler { url in
            guard let url, let link = URL(string: String(cString: url)) else { return }
            DispatchQueue.main.async { UIApplication.shared.open(link) }
        }
        husk_cocos_set_keyboard_handler { action in
            DispatchQueue.main.async {
                guard let view = TLUnityUIView.cocosView else { return }
                let show = action == 1 || (action == 0 && !view.isFirstResponder)
                if show {
                    // Start the strip from what the game's field already holds, so editing a name shows the whole name.
                    husk_cocos_request_text { text in
                        let seed = text.map { String(cString: $0) } ?? ""
                        DispatchQueue.main.async { TLUnityUIView.cocosView?.setTyped(seed) }
                    }
                    view.becomeFirstResponder()
                } else {
                    view.resignFirstResponder()
                }
            }
        }
    }

    /// A GameActivity game's requests (Minecraft's text fields, through GameTextInput): 1 shows the keyboard, 2 hides it.
    static func installGameActivityKeyboardHandler() {
        husk_ga_set_keyboard_handler { action in
            DispatchQueue.main.async {
                guard let view = TLUnityUIView.cocosView else { return }
                if action == 1 {
                    var buf = [CChar](repeating: 0, count: 16384)
                    husk_ga_text(&buf, UInt(buf.count))
                    view.setTyped(String(cString: buf))
                    view.becomeFirstResponder()
                } else {
                    view.resignFirstResponder()
                }
            }
        }
    }

    /// An SDL game's requests (SDL_StartTextInput / SDL_StopTextInput, from its main thread): 1 shows the keyboard, 2 hides it.
    static func installSDLKeyboardHandler() {
        husk_sdl_set_keyboard_handler { action in
            DispatchQueue.main.async {
                guard let view = TLUnityUIView.cocosView else { return }
                if action == 1 { view.setTyped(""); view.becomeFirstResponder() } else { view.resignFirstResponder() }
            }
        }
    }

    // MARK: touch

    private func id(for touch: UITouch) -> Int32 {
        let key = ObjectIdentifier(touch)
        if let existing = pointers[key] { return existing }
        var next: Int32 = 0
        while pointers.values.contains(next) { next += 1 }
        pointers[key] = next
        return next
    }

    private func send(_ touches: Set<UITouch>, phase: Int32) {
        for t in touches {
            let p = t.location(in: self)
            let pid = id(for: t)
            husk_unity_touch(phase, pid, Float(p.x * contentScaleFactor), Float(p.y * contentScaleFactor))
            if phase == 2 || phase == 3 { pointers[ObjectIdentifier(t)] = nil }
        }
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?)     { send(touches, phase: 0) }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?)     { send(touches, phase: 1) }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?)     { send(touches, phase: 2) }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        send(touches, phase: 2)
        pointers.removeAll()
    }
}

struct TLUnityScreen: UIViewRepresentable {
    let apk: String
    var extraApks: [String] = []
    let dataDir: String
    var engine: TLNativeEngine = .unity
    var portrait = false
    var scale: CGFloat = 2
    var onThreeFingerTap: (() -> Void)? = nil
    var onTopTap: (() -> Void)? = nil
    /// One view per game for the life of the process. The engine's GPU surface belongs to this view's layer and an
    /// engine cannot be started twice, so coming back to the game must show the same layer, not a new one.
    private static var shared: [String: TLUnityUIView] = [:]

    func makeUIView(context: Context) -> TLUnityUIView {
        let view = Self.shared[apk]
            ?? TLUnityUIView(apk: apk, extraApks: extraApks, dataDir: dataDir, engine: engine, portrait: portrait, scale: scale)
        Self.shared[apk] = view
        updateUIView(view, context: context)
        return view
    }

    func updateUIView(_ view: TLUnityUIView, context: Context) {
        view.onThreeFingerTap = onThreeFingerTap
        view.onTopTap = onTopTap
    }
}

/// Polls the runtime for the status line and its log, ten times a second at most.
@MainActor
final class TLUnityModel: ObservableObject {
    @Published var state: Int32 = 0
    @Published var frames: UInt = 0
    @Published var logText = ""
    private var timer: Timer?
    private var ticks = 0

    func start() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.poll() }
        }
    }

    func stop() { timer?.invalidate(); timer = nil }

    private func poll() {
        ticks += 1
        state = husk_unity_state()
        frames = husk_unity_frames()
        if ticks % 4 == 0, let c = husk_tl_attempt_log() {
            let text = String(cString: c)
            free(c)
            if text != logText { logText = text }
        }
    }

    var statusText: String {
        switch state {
        case Int32(HUSK_UNITY_STARTING): return "Loading the engine…"
        case Int32(HUSK_UNITY_RUNNING):  return "Running"
        case Int32(HUSK_UNITY_FAILED):   return "Could not start"
        case Int32(HUSK_UNITY_ENDED):    return "The game exited"
        default:                         return "Starting"
        }
    }

    var subStatusText: String {
        switch state {
        case Int32(HUSK_UNITY_RUNNING): return "\(frames) frame(s) drawn · native runtime"
        case Int32(HUSK_UNITY_STARTING): return "Loading libraries and starting the engine"
        default: return "Native runtime"
        }
    }

    var statusColor: Color {
        switch state {
        case Int32(HUSK_UNITY_RUNNING):  return Theme.good
        case Int32(HUSK_UNITY_FAILED):   return Theme.bad
        case Int32(HUSK_UNITY_ENDED):    return Theme.warn
        default:                         return Theme.accent
        }
    }
}

/// A game the native runtime drives -- Unity, cocos2d-x, GameActivity, SDL, Rockstar, Unreal, Godot, NativeActivity --
/// full screen, with nothing over it. Tapping the top of the screen brings a toolbar down: the way out, what is running, and
/// the game's own controls. It goes again a few seconds later. Three fingers tapped together do the same.
struct TLCocosAttemptView: View {
    let app: TLApp
    @Environment(\.dismiss) private var dismiss
    @StateObject private var model = TLUnityModel()
    @StateObject private var monitor = PerformanceMonitor()
    /// The run log over the game, opened from the toolbar.
    @State private var showLog = false
    @ObservedObject private var pads = HuskGamepads.shared
    @StateObject private var virtualPad = VirtualPad()
    /// Where the player has put the pad's controls in this game, and whether they are moving them now.
    @State private var padLayout = PadLayout()
    @State private var editingPad = false
    @State private var padSelected: String?
    /// This game's own settings (TLAppSettings), read once as the screen opens.
    @State private var settings: TLAppSettings
    /// Whether the toolbar is down.
    @State private var chrome = false
    /// Whether the game was laid out over the whole screen, the area around the camera included (the game's Full Screen
    /// setting). The game's surface is sized once at launch, so this cannot change while it runs.
    private let fullBleed: Bool

    init(app: TLApp) {
        self.app = app
        let loaded = TLAppSettings.load(app.id)
        _settings = State(initialValue: loaded)
        fullBleed = loaded.cleanView
    }

    /// Geometry Dash and the like are cocos2d-x; Minecraft is built on GameActivity. Both are landscape.
    private var engine: TLNativeEngine {
        switch app.report?.nativeEngine {
        case .minecraft: return .minecraft
        case .sdl: return .sdl
        case .ue4: return .ue4
        case .gta: return .gta
        case .godot: return .godot
        case .nativeactivity: return .nativeactivity
        case .cocos: return .cocos
        default: return .unity
        }
    }

    private var dataDir: String {
        TranslationLayer.root.appendingPathComponent(app.id, isDirectory: true)
            .appendingPathComponent(engine == .unity ? "unity-data" : engine == .minecraft ? "minecraft-data" : engine == .sdl ? "sdl-data" : engine == .ue4 ? "ue4-data" : engine == .gta ? "gta-data" : engine == .godot ? "godot-data" : engine == .nativeactivity ? "na-data" : "cocos-data", isDirectory: true).path
    }

    /// Which way up: what the game's settings say, and otherwise what its manifest asks. Some SDL games are portrait; every other
    /// native game is landscape.
    private var portrait: Bool {
        switch settings.orientation {
        case .landscape: return false
        case .portrait: return true
        case .auto:
            guard engine == .sdl || engine == .nativeactivity || engine == .unity || engine == .godot, let apk = app.apks.first else { return false }
            return husk_sdl_apk_is_portrait(apk) != 0
        }
    }

    /// Whether an on-screen controller may be offered: not when the game's settings say never, not while a real one is connected, and
    /// without "Always" only for Unreal, whose menus answer nothing else.
    private var padOffered: Bool {
        settings.pad != .never && pads.names.isEmpty && (settings.pad == .always || engine == .ue4)
    }

    /// Another game is already loaded in this session, and an engine cannot be loaded twice.
    private var blockedBy: String? {
        guard let loaded = husk_native_loaded_apk().map({ String(cString: $0) }), loaded != app.apks.first else { return nil }
        return (loaded as NSString).lastPathComponent
    }

    /// The game is not running and is not going to: nothing to hide the toolbar for.
    private var stopped: Bool {
        blockedBy != nil || model.state == Int32(HUSK_UNITY_FAILED) || model.state == Int32(HUSK_UNITY_ENDED)
    }

    private var statusLine: String {
        guard model.state == Int32(HUSK_UNITY_RUNNING) else { return model.statusText }
        if !pads.names.isEmpty { return pads.names.count == 1 ? pads.names[0] : "\(pads.names.count) controllers" }
        return "\(app.report?.nativeEngineName ?? "Native") · running"
    }

    private func toggleChrome() {
        withAnimation(.snappy(duration: 0.25)) { chrome.toggle() }
    }

    var body: some View {
        ZStack {
            // The band above the game when it keeps clear of the camera: tapping it is tapping the top of the screen.
            Color.black.ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture { toggleChrome() }

            if let other = blockedBy {
                VStack(spacing: 8) {
                    Image(systemName: "rectangle.stack.badge.minus")
                        .font(.system(size: 30, weight: .medium)).foregroundStyle(.white.opacity(0.7))
                        .padding(.bottom, 4)
                    Text("Another game is already loaded")
                        .font(.system(size: 17, weight: .semibold)).foregroundStyle(.white)
                    Text("\(other) was started in this session, and a game cannot be unloaded once it has started. Close Husk completely and open it again to run \(app.label).")
                        .font(.system(size: 14)).foregroundStyle(.white.opacity(0.7))
                        .multilineTextAlignment(.center).frame(maxWidth: 440)
                    Label("Tap the top of the screen to close", systemImage: "hand.tap")
                        .font(.system(size: 13, weight: .medium)).foregroundStyle(.white.opacity(0.55))
                        .padding(.top, 6)
                }
                .padding(24)
                .allowsHitTesting(false)
            } else if let apk = app.apks.first {
                TLUnityScreen(apk: apk, extraApks: Array(app.apks.dropFirst()), dataDir: dataDir, engine: engine, portrait: portrait,
                              scale: settings.resolution.scale,
                              onThreeFingerTap: { toggleChrome() }, onTopTap: { toggleChrome() })
                    .background(Color.black)
                    .overlay {
                        // A game whose menus answer only a controller: with none paired, one on the glass.
                        if padOffered, settings.padShown, model.state == Int32(HUSK_UNITY_RUNNING) {
                            VirtualPadView(pad: virtualPad, opacity: settings.padOpacity, haptics: settings.haptics,
                                           layout: padLayout, editing: editingPad, selected: $padSelected,
                                           onChange: { padLayout = $0; padLayout.save(app.id) })
                                .overlay(alignment: .center) { if editingPad { padEditor } }
                        }
                    }
                    .ignoresSafeArea(.container, edges: fullBleed ? .all : [.horizontal, .bottom])
            }

            // A game that did not start, or that quit: say so, rather than leave a black screen.
            if blockedBy == nil, model.state == Int32(HUSK_UNITY_FAILED) || model.state == Int32(HUSK_UNITY_ENDED) {
                VStack(spacing: 8) {
                    Image(systemName: model.state == Int32(HUSK_UNITY_FAILED) ? "exclamationmark.triangle" : "stop.circle")
                        .font(.system(size: 28, weight: .medium)).foregroundStyle(.white.opacity(0.7))
                    Text(model.state == Int32(HUSK_UNITY_FAILED) ? "\(app.label) could not start" : "\(app.label) exited")
                        .font(.system(size: 17, weight: .semibold)).foregroundStyle(.white)
                    Label("Tap the top of the screen to close or see the log", systemImage: "hand.tap")
                        .font(.system(size: 13, weight: .medium)).foregroundStyle(.white.opacity(0.55))
                }
                .multilineTextAlignment(.center)
                .padding(24)
                .allowsHitTesting(false)
            }

            if showLog {
                GameLogPanel(text: model.logText) { withAnimation(.snappy(duration: 0.25)) { showLog = false } }
                    .frame(maxWidth: 360)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .trailing)
                    .padding(.top, 64).padding(.trailing, 10).padding(.bottom, 10)
                    .transition(.move(edge: .trailing).combined(with: .opacity))
            }

            GameOverlay(title: app.label, status: statusLine, statusColor: model.statusColor, shown: $chrome,
                        pinned: editingPad || showLog || stopped, monitor: monitor, onClose: { dismiss() }) {
                if padOffered {
                    OverlayButton(systemImage: "gamecontroller", label: settings.padShown ? "Hide Controller" : "Show Controller",
                                  active: settings.padShown) {
                        settings.padShown.toggle()
                        settings.save(app.id)
                        if !settings.padShown { editingPad = false }
                    }
                    if settings.padShown {
                        OverlayButton(systemImage: "slider.horizontal.below.square.and.square.filled", label: "Edit Controller",
                                      active: editingPad) { padSelected = nil; editingPad.toggle() }
                    }
                }
                PerfOverlayButton()
                OverlayButton(systemImage: "doc.text.magnifyingglass", label: showLog ? "Hide Log" : "Show Log",
                              active: showLog) {
                    withAnimation(.snappy(duration: 0.25)) { showLog.toggle() }
                }
            }
        }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
        // Swipes near the edges are the game's.
        .defersSystemGestures(on: .all)
        .onAppear {
            HuskOrientation.set(portrait ? .portrait : .landscape)
            UIApplication.shared.isIdleTimerDisabled = settings.keepAwake
            CrashReport.gameStarted(app)
            padLayout = PadLayout.load(app.id)
            model.start()
            monitor.start(.native)
        }
        // What happened, for the library: ten seconds of frames is a game that plays; a refusal is one that did not start.
        .onChange(of: model.frames) { f in
            if f > 600, model.state == Int32(HUSK_UNITY_RUNNING) { GameStatusStore.shared.record(app.id, .plays) }
        }
        .onChange(of: model.state) { st in
            if st == Int32(HUSK_UNITY_FAILED) { GameStatusStore.shared.record(app.id, .failed) }
        }
        .onDisappear {
            CrashReport.gameEnded()
            model.stop()
            monitor.stop()
            UIApplication.shared.isIdleTimerDisabled = false
            HuskOrientation.set(HuskOrientation.standard)
        }
    }

    /// While the pad is being edited: what to do with the control picked, in a small panel in the middle of the screen.
    private var padEditor: some View {
        VStack(spacing: 12) {
            if let id = padSelected {
                Text(PadLayout.name(id)).font(.system(size: 15, weight: .semibold)).foregroundStyle(.white)
                HStack(spacing: 10) {
                    Image(systemName: "minus.magnifyingglass").foregroundStyle(.white.opacity(0.7))
                    Slider(value: Binding(get: { Double(padLayout[id].scale) },
                                          set: { padLayout[id].scale = CGFloat($0) }),
                           in: 0.6...1.8, onEditingChanged: { if !$0 { padLayout.save(app.id) } })
                        .frame(width: 180)
                    Image(systemName: "plus.magnifyingglass").foregroundStyle(.white.opacity(0.7))
                }
                HStack(spacing: 10) {
                    Button(padLayout[id].hidden ? "Show" : "Hide") { padLayout[id].hidden.toggle(); padLayout.save(app.id) }
                    Button("Reset") { padLayout[id] = PadLayout.Adjust(); padLayout.save(app.id) }
                }
                .buttonStyle(.bordered).tint(.white)
            } else {
                Text("Drag a control to move it, or tap one to resize or hide it.")
                    .font(.system(size: 14, weight: .medium)).foregroundStyle(.white)
                    .multilineTextAlignment(.center).frame(maxWidth: 260)
            }
            HStack(spacing: 10) {
                Button("Reset All", role: .destructive) { padLayout = PadLayout(); padSelected = nil; padLayout.save(app.id) }
                Button("Done") { editingPad = false; padSelected = nil }.buttonStyle(.borderedProminent)
            }
            .buttonStyle(.bordered)
        }
        .padding(18)
        .huskPanel(RoundedRectangle(cornerRadius: 20, style: .continuous))
        .font(.system(size: 13, weight: .semibold))
    }
}
