// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

// MARK: - the performance overlay's settings

/// The frame-rate readout over a running game: off unless turned on in Settings (or from a game's toolbar), and drawn in the
/// corner the person picks. It is the same for every game.
/// Which screen edges a game keeps for itself. The bottom always is (a swipe up there would leave the game for the home screen).
/// The top is only when asked: guarding it means iOS shows its pill with an arrow on the first swipe down and opens Notification
/// Center or Control Center only on a second one, and apps cannot show one without the other.
enum GameEdges {
    static let guardTopKey = "husk.game.guardTopEdge"
    static func deferred(guardTop: Bool) -> Edge.Set { guardTop ? .all : [.bottom, .leading, .trailing] }
}

enum PerfOverlay {
    static let enabledKey = "husk.perfOverlay"
    static let positionKey = "husk.perfOverlay.position"
    static let detailedKey = "husk.perfOverlay.detailed"

    enum Position: String, CaseIterable, Identifiable {
        case topLeading, topTrailing, bottomLeading, bottomTrailing
        var id: String { rawValue }
        var title: String {
            switch self {
            case .topLeading: return "Top Left"
            case .topTrailing: return "Top Right"
            case .bottomLeading: return "Bottom Left"
            case .bottomTrailing: return "Bottom Right"
            }
        }
        var alignment: Alignment {
            switch self {
            case .topLeading: return .topLeading
            case .topTrailing: return .topTrailing
            case .bottomLeading: return .bottomLeading
            case .bottomTrailing: return .bottomTrailing
            }
        }
        var isTop: Bool { self == .topLeading || self == .topTrailing }
    }
}

// MARK: - what the overlay reads

/// How the game on screen is doing, sampled once a second while it runs.
///
/// The native runtime's counters reset each time they are read ("since the last call"), so this is the only thing that reads
/// them: two readers would each see half the frames.
@MainActor
final class PerformanceMonitor: ObservableObject {
    enum Source { case native, classic }

    /// Frames the game finished per second.
    @Published private(set) var fps: Double = 0
    /// The mean time one frame takes the game, in milliseconds.
    @Published private(set) var frameMs: Double = 0
    /// The slowest frame in the last second, when the runtime measures it.
    @Published private(set) var worstMs: Double = 0
    /// What the process holds in memory: the number iOS ends an app on.
    @Published private(set) var memory: UInt64 = 0
    @Published private(set) var thermal: ProcessInfo.ThermalState = .nominal

    private var timer: Timer?
    private var source: Source = .native

    func start(_ source: Source) {
        self.source = source
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.sample() }
        }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    private func sample() {
        switch source {
        case .native:
            var p = husk_unity_perf()
            husk_unity_perf_snapshot(&p)
            fps = p.fps
            frameMs = p.mean_ms
            worstMs = p.max_ms
        case .classic:
            var p = husk_tl_perf()
            husk_tl_perf_snapshot(&p)
            fps = p.fps
            frameMs = p.logic_ms + p.render_ms
            worstMs = 0
        }
        memory = Self.footprint()
        thermal = ProcessInfo.processInfo.thermalState
    }

    /// The process's physical footprint, as task_info reports it.
    nonisolated static func footprint() -> UInt64 {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let kr = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        return kr == KERN_SUCCESS ? info.phys_footprint : 0
    }
}

/// The readout itself: a small dark pill, never in the way of a touch.
struct PerformanceHUD: View {
    @ObservedObject var monitor: PerformanceMonitor
    var detailed = true

    private var fpsColor: Color {
        if monitor.fps <= 0 { return .gray }
        if monitor.fps >= 50 { return .green }
        if monitor.fps >= 25 { return .yellow }
        return .red
    }

    private var thermalSymbol: (String, Color)? {
        switch monitor.thermal {
        case .fair: return ("thermometer.medium", .yellow)
        case .serious: return ("thermometer.high", .orange)
        case .critical: return ("flame.fill", .red)
        default: return nil
        }
    }

    var body: some View {
        HStack(spacing: 7) {
            Circle().fill(fpsColor).frame(width: 6, height: 6)
            Text(monitor.fps > 0 ? String(format: "%.0f FPS", monitor.fps) : "— FPS")
            if detailed {
                separator
                Text(monitor.frameMs > 0 ? String(format: "%.1f ms", monitor.frameMs) : "— ms")
                if monitor.worstMs > 0 {
                    Text(String(format: "max %.0f", monitor.worstMs)).foregroundStyle(.white.opacity(0.6))
                }
                separator
                Text(ByteCountFormatter.string(fromByteCount: Int64(monitor.memory), countStyle: .memory))
            }
            if let heat = thermalSymbol {
                Image(systemName: heat.0).foregroundStyle(heat.1)
            }
        }
        .font(.technical(11, weight: .semibold))
        .foregroundStyle(.white)
        .monospacedDigit()
        .padding(.horizontal, 10)
        .frame(height: 26)
        .huskPanel(Capsule())
        .allowsHitTesting(false)
        .accessibilityElement(children: .combine)
        .accessibilityLabel("Performance")
    }

    private var separator: some View {
        Rectangle().fill(.white.opacity(0.25)).frame(width: 0.5, height: 12)
    }
}

// MARK: - the toolbar

/// The bar that comes down over a game on a three-finger tap: the way out, what is running, and the game's
/// own controls. It hides itself a few seconds later.
struct GameToolbar<Actions: View>: View {
    let title: String
    let status: String
    let statusColor: Color
    let onClose: () -> Void
    @ViewBuilder var actions: Actions

    var body: some View {
        HStack(spacing: 10) {
            OverlayButton(systemImage: "xmark", label: "Close Game", action: onClose)
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.system(size: 14, weight: .semibold))
                    .lineLimit(1)
                HStack(spacing: 5) {
                    Circle().fill(statusColor).frame(width: 6, height: 6)
                    Text(status)
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(.white.opacity(0.7))
                        .lineLimit(1)
                }
            }
            .foregroundStyle(.white)
            Spacer(minLength: 12)
            HStack(spacing: 8) { actions }
        }
        .padding(6)
        .padding(.trailing, 2)
        .huskPanel(Capsule())
        .frame(maxWidth: 620)
    }
}

/// Everything drawn over a running game, in one layer: the toolbar (shown only after a three-finger tap, and
/// hidden again after a few seconds unless `pinned`), the performance readout, and for the first few games a line saying how
/// to bring the toolbar up.
struct GameOverlay<Actions: View>: View {
    let title: String
    let status: String
    let statusColor: Color
    @Binding var shown: Bool
    /// Something on screen needs the toolbar to stay: the controller editor, the log.
    var pinned = false
    @ObservedObject var monitor: PerformanceMonitor
    let onClose: () -> Void
    @ViewBuilder var actions: Actions

    @AppStorage(PerfOverlay.enabledKey) private var perfOn = false
    @AppStorage(PerfOverlay.positionKey) private var position: PerfOverlay.Position = .topLeading
    @AppStorage(PerfOverlay.detailedKey) private var detailed = true
    @State private var hint = false
    /// How many games have shown the hint. It is only for learning the gesture, so it stops after a few.
    @AppStorage("husk.game.threeFingerHintCount") private var hintCount = 0
    /// Changes whenever the toolbar is shown, which restarts the wait before it hides.
    @State private var revealed = 0

    var body: some View {
        ZStack {
            if perfOn {
                PerformanceHUD(monitor: monitor, detailed: detailed)
                    // Below the toolbar while it is down, so the two never overlap.
                    .padding(.top, position.isTop && shown ? 64 : 0)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: position.alignment)
                    .padding(10)
                    .animation(.snappy(duration: 0.25), value: shown)
            }

            VStack(spacing: 0) {
                if shown {
                    GameToolbar(title: title, status: status, statusColor: statusColor, onClose: onClose) { actions }
                        .padding(.horizontal, 10)
                        .padding(.top, 8)
                        .transition(.move(edge: .top).combined(with: .opacity))
                }
                Spacer(minLength: 0)
                if hint {
                    Label("Tap with three fingers for controls", systemImage: "hand.tap")
                        .font(.system(size: 13, weight: .medium))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 14).frame(height: 34)
                        .huskPanel(Capsule())
                        .padding(.bottom, 22)
                        .transition(.opacity)
                        .allowsHitTesting(false)
                }
            }
        }
        .animation(.snappy(duration: 0.25), value: shown)
        .onChange(of: shown) { if $0 { revealed += 1 } }
        .task(id: revealed) {
            guard shown else { return }
            try? await Task.sleep(nanoseconds: 4_500_000_000)
            guard !Task.isCancelled, !pinned else { return }
            shown = false
        }
        .onChange(of: pinned) { stillPinned in
            // Unpinned while down: give it the usual few seconds from here.
            if !stillPinned, shown { revealed += 1 }
        }
        .onAppear {
            guard hintCount < 3 else { return }
            hintCount += 1
            withAnimation(.easeOut(duration: 0.3)) { hint = true }
            DispatchQueue.main.asyncAfter(deadline: .now() + 3.5) {
                withAnimation(.easeOut(duration: 0.4)) { hint = false }
            }
        }
    }
}

/// The quick switch for the performance readout, for a game's toolbar.
struct PerfOverlayButton: View {
    @AppStorage(PerfOverlay.enabledKey) private var perfOn = false

    var body: some View {
        OverlayButton(systemImage: "speedometer", label: perfOn ? "Hide Performance" : "Show Performance",
                      active: perfOn) { perfOn.toggle() }
    }
}

/// The game's run log over the game: what the runtime did to start it, live, with ways to copy or share it. Opened from the
/// game's toolbar.
struct GameLogPanel: View {
    let text: String
    let onClose: () -> Void
    @State private var copied = false

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Text("Log")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(.white)
                Spacer()
                Button {
                    UIPasteboard.general.string = text
                    copied = true
                    DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { copied = false }
                } label: {
                    Label(copied ? "Copied" : "Copy", systemImage: copied ? "checkmark" : "doc.on.doc")
                        .font(.system(size: 12, weight: .semibold))
                }
                .tint(.white)
                ShareLink(item: text) {
                    Label("Share", systemImage: "square.and.arrow.up")
                        .font(.system(size: 12, weight: .semibold))
                }
                .tint(.white)
                Button(action: onClose) {
                    Image(systemName: "xmark")
                        .font(.system(size: 11, weight: .bold))
                        .foregroundStyle(.white)
                        .frame(width: 26, height: 26)
                        .background(Color.white.opacity(0.12), in: Circle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Close Log")
            }
            .padding(.horizontal, 12).padding(.vertical, 8)
            Rectangle().fill(Color.white.opacity(0.12)).frame(height: 0.5)
            ScrollViewReader { proxy in
                ScrollView {
                    Text(text.isEmpty ? "Starting…" : text)
                        .font(.technical(10))
                        .foregroundStyle(.white.opacity(0.85))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(12)
                        .textSelection(.enabled)
                        .id("bottom")
                }
                .onChange(of: text) { _ in proxy.scrollTo("bottom", anchor: .bottom) }
                .onAppear { proxy.scrollTo("bottom", anchor: .bottom) }
            }
        }
        .huskPanel(RoundedRectangle(cornerRadius: 18, style: .continuous))
        .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
    }
}

