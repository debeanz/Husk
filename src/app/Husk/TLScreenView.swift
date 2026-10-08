// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

/// The translation layer's screen, shown the way a game needs to be shown.
///
/// The first version put each frame into SwiftUI: every sixtieth of a second a
/// timer copied two megabytes into a `Data`, made a `UIImage` out of it, and
/// published it, which re-evaluated the whole attempt view -- log and all -- and
/// pushed a new image through SwiftUI's diffing. None of that is work a game
/// frame should cost, and the timer was not tied to the display at all, so
/// frames were shown at whatever beat it happened to fall into.
///
/// This is a plain `UIView` holding one `CALayer`, driven by a `CADisplayLink`.
/// On each refresh it takes the newest finished frame -- pinned, not copied --
/// and sets it as the layer's contents. SwiftUI sees nothing change, so nothing
/// is re-evaluated; the only per-frame work is making a `CGImage` that points at
/// memory the producer has promised not to touch until it is released.
final class TLScreenUIView: UIView, UIGestureRecognizerDelegate {
    private let content = CALayer()
    private var link: CADisplayLink?
    private var lastGeneration: UInt64 = 0

    /// What the guest's frame is, so a touch can be mapped back into it.
    private let guestSize = CGSize(width: 540, height: 960)

    /// Three fingers tapped at once: another way to bring the game's toolbar up or put it away.
    var onThreeFingerTap: (() -> Void)?
    /// Called when a finger pulls down from the top edge: the way to bring the game's toolbar down. A tap at the top is the
    /// game's alone -- games put buttons there.
    var onPullDown: (() -> Void)?
    /// How close to the top edge the pull has to start, in points.
    var pullEdge: CGFloat = 24
    private var topPull: UIPanGestureRecognizer?
    private var pullFired = false
    /// The one finger the game is following; a second or third finger is only for the three-finger tap.
    private var primary: UITouch?

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
        // Several fingers reach the view so a three-finger tap can be seen; the game itself only ever gets the first.
        isMultipleTouchEnabled = true
        let three = UITapGestureRecognizer(target: self, action: #selector(threeFingers))
        three.numberOfTouchesRequired = 3
        three.cancelsTouchesInView = false
        three.delaysTouchesBegan = false
        three.delaysTouchesEnded = false
        addGestureRecognizer(three)

        // A pull down from the top edge brings the toolbar down. It watches without taking anything: the game still gets
        // every touch, exactly as it would with no recognizer there.
        let pull = UIPanGestureRecognizer(target: self, action: #selector(pulled(_:)))
        pull.maximumNumberOfTouches = 1
        pull.cancelsTouchesInView = false
        pull.delaysTouchesBegan = false
        pull.delaysTouchesEnded = false
        pull.delegate = self
        addGestureRecognizer(pull)
        topPull = pull

        // Opaque, so Core Animation does not blend it with what is behind it.
        content.isOpaque = true
        content.magnificationFilter = .linear
        content.minificationFilter = .linear
        content.actions = ["contents": NSNull(), "bounds": NSNull(), "position": NSNull()]
        layer.addSublayer(content)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    deinit {
        link?.invalidate()
    }

    // MARK: layout

    /// The largest rectangle with the guest's shape that fits, centred.
    private var fitted: CGRect {
        let a = guestSize.width / guestSize.height
        var w = bounds.width, h = bounds.height
        if w / max(h, 1) > a { w = h * a } else { h = w / a }
        return CGRect(x: (bounds.width - w) / 2, y: (bounds.height - h) / 2, width: w, height: h)
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        content.frame = fitted
    }

    // MARK: presenting

    override func didMoveToWindow() {
        super.didMoveToWindow()
        link?.invalidate()
        link = nil
        guard window != nil else { return }

        // Tied to the display, not to a timer: a frame is shown on a refresh or
        // not at all. Sixty is the game's own rate, and asking for no more lets a
        // ProMotion screen settle at a rate that divides evenly into it.
        let l = CADisplayLink(target: self, selector: #selector(refresh))
        l.preferredFrameRateRange = CAFrameRateRange(minimum: 30, maximum: 60, preferred: 60)
        l.add(to: .main, forMode: .common)
        link = l
    }

    @objc private func refresh() {
        var pixels: UnsafePointer<UInt8>?
        var w: Int32 = 0, h: Int32 = 0, stride: Int32 = 0, token: Int32 = -1
        let generation = husk_tl_frame_acquire(&pixels, &w, &h, &stride, &token)
        guard generation != 0, let px = pixels else { return }

        // Nothing new since the last refresh: let go of it and keep what is shown.
        if generation == lastGeneration {
            husk_tl_frame_release(token)
            return
        }
        lastGeneration = generation

        let bytesPerRow = Int(stride) * 4
        // The provider owns the pin. The slot goes back to the producer when
        // Core Animation has finished with the image, not before: releasing it
        // here would let the guest overwrite pixels still being uploaded.
        let info = UnsafeMutableRawPointer(bitPattern: Int(token) + 1)
        guard let provider = CGDataProvider(dataInfo: info, data: px, size: bytesPerRow * Int(h),
                                            releaseData: { info, _, _ in
            if let info { husk_tl_frame_release(Int32(Int(bitPattern: info) - 1)) }
        }) else {
            husk_tl_frame_release(token)
            return
        }

        // The alpha byte is ignored: the guest's frame is opaque, and telling
        // Core Animation so is what lets it skip blending.
        let bitmap = CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue
                                          | CGBitmapInfo.byteOrder32Big.rawValue)
        guard let image = CGImage(width: Int(w), height: Int(h), bitsPerComponent: 8,
                                  bitsPerPixel: 32, bytesPerRow: bytesPerRow,
                                  space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: bitmap,
                                  provider: provider, decode: nil, shouldInterpolate: false,
                                  intent: .defaultIntent) else { return }

        CATransaction.begin()
        CATransaction.setDisableActions(true)
        content.contents = image
        CATransaction.commit()
    }

    // MARK: touch

    /// Where a point in this view is in the guest's own pixels.
    private func guest(_ p: CGPoint) -> (Float, Float) {
        let r = fitted
        let x = max(0, min(1, (p.x - r.minX) / max(r.width, 1)))
        let y = max(0, min(1, (p.y - r.minY) / max(r.height, 1)))
        return (Float(x * guestSize.width), Float(y * guestSize.height))
    }

    /// One touch, with the three phases Android's input has: down once, move as
    /// it moves, up once. The earlier gesture sent DOWN on every change and never
    /// a move, so a finger that wobbled while tapping delivered several downs --
    /// and in a game where a down is a flap, several flaps from one tap.
    private func send(_ t: UITouch, action: Int32) {
        let (x, y) = guest(t.location(in: self))
        husk_tl_send_touch(action, x, y)
    }

    @objc private func threeFingers() { onThreeFingerTap?() }
    /// Down far enough, and more down than sideways: the toolbar, once per pull.
    @objc private func pulled(_ g: UIPanGestureRecognizer) {
        switch g.state {
        case .changed:
            let t = g.translation(in: self)
            if !pullFired, t.y > 36, t.y > abs(t.x) * 1.5 {
                pullFired = true
                onPullDown?()
            }
        default:
            pullFired = false
        }
    }

    func gestureRecognizer(_ g: UIGestureRecognizer, shouldReceive touch: UITouch) -> Bool {
        guard g === topPull else { return true }
        return touch.location(in: self).y <= pullEdge
    }

    func gestureRecognizer(_ g: UIGestureRecognizer,
                           shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool { true }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard primary == nil, let t = touches.first else { return }
        primary = t
        send(t, action: 0)
    }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        if let p = primary, touches.contains(p) { send(p, action: 2) }
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        if let p = primary, touches.contains(p) { send(p, action: 1); primary = nil }
    }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        if let p = primary, touches.contains(p) { send(p, action: 3); primary = nil }
    }
}

struct TLScreenView: UIViewRepresentable {
    var onThreeFingerTap: (() -> Void)? = nil
    var onPullDown: (() -> Void)? = nil

    func makeUIView(context: Context) -> TLScreenUIView {
        let view = TLScreenUIView()
        updateUIView(view, context: context)
        return view
    }
    func updateUIView(_ view: TLScreenUIView, context: Context) {
        view.onThreeFingerTap = onThreeFingerTap
        view.onPullDown = onPullDown
    }
}
