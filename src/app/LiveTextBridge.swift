// SPDX-License-Identifier: GPL-3.0-or-later
// F-MAC-06 (M4): VisionKit's ImageAnalyzer and ImageAnalysisOverlayView exist only as Swift
// API on macOS. This small @objc class makes them usable from Objective-C++
// (LiveTextOverlay_mac.mm declares its interface). Compiled by src/CMakeLists.txt with
// swiftc into an object of lf_ui; no UI texts here (they come from the C++ side).

import AppKit
import VisionKit

@MainActor @objc(LFLiveTextBridge) public final class LiveTextBridge: NSObject, ImageAnalysisOverlayViewDelegate {
    private let analyzer = ImageAnalyzer()
    private let overlay = ImageAnalysisOverlayView()
    private var task: Task<Void, Never>?
    // Image area inside the overlay in unit coordinates, origin top left (Qt convention).
    private var topLeftRect = CGRect(x: 0, y: 0, width: 1, height: 1)

    @objc public override init() {
        super.init()
        overlay.delegate = self
        overlay.preferredInteractionTypes = .automatic // text, data detectors, subject, Visual Look Up
        overlay.autoresizingMask = []
    }

    @objc public var view: NSView { overlay }

    @objc public static var isSupported: Bool { ImageAnalyzer.isSupported }

    @objc public func setContentsRect(_ rect: CGRect) {
        topLeftRect = rect
        overlay.setContentsRectNeedsUpdate()
    }

    public func contentsRect(for overlayView: ImageAnalysisOverlayView) -> CGRect {
        if overlayView.isFlipped {
            return topLeftRect
        }
        return CGRect(x: topLeftRect.minX, y: 1.0 - topLeftRect.maxY, width: topLeftRect.width,
                      height: topLeftRect.height)
    }

    // Analyses `image` (text, Visual Look Up, codes); `completion(hasResults, error)` runs on the
    // main thread unless the analysis was cancelled meanwhile.
    @objc public func analyze(_ image: CGImage, completion: @escaping (Bool, String?) -> Void) {
        task?.cancel()
        overlay.analysis = nil
        task = Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let types: ImageAnalyzer.AnalysisTypes = [.text, .visualLookUp, .machineReadableCode]
                let result = try await self.analyzer.analyze(image, orientation: .up,
                                                             configuration: ImageAnalyzer.Configuration(types))
                if Task.isCancelled { return }
                self.overlay.analysis = result
                completion(result.hasResults(for: types), nil)
            } catch {
                if !Task.isCancelled { completion(false, error.localizedDescription) }
            }
        }
    }

    @objc public func cancel() {
        task?.cancel()
        task = nil
        overlay.analysis = nil
    }

    @objc public var hasActiveTextSelection: Bool { overlay.hasActiveTextSelection }
}
