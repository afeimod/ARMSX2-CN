// SwiftUIHost.swift — ObjC-callable helper to create SwiftUI hosting controllers
// SPDX-License-Identifier: GPL-3.0+

import SwiftUI
import UIKit

/// Custom hosting controller that respects fullScreen state for status bar hiding
class ARMSX2HostingController<Content: View>: UIHostingController<Content> {
    override var prefersStatusBarHidden: Bool {
        AppState.shared.hideStatusBar
    }
    override var prefersHomeIndicatorAutoHidden: Bool {
        AppState.shared.hideStatusBar || AppState.shared.hideHomeIndicator
    }
    override var preferredStatusBarUpdateAnimation: UIStatusBarAnimation {
        .fade
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(systemChromeNeedsUpdate),
            name: AppState.systemChromeNeedsUpdateNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(systemChromeNeedsUpdate),
            name: UIApplication.didBecomeActiveNotification,
            object: nil
        )
    }

    deinit {
        NotificationCenter.default.removeObserver(
            self,
            name: AppState.systemChromeNeedsUpdateNotification,
            object: nil
        )
        NotificationCenter.default.removeObserver(
            self,
            name: UIApplication.didBecomeActiveNotification,
            object: nil
        )
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        applyNativeContentScale(to: view)
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        applyNativeContentScale(to: view)
    }

    @objc private func systemChromeNeedsUpdate() {
        setNeedsStatusBarAppearanceUpdate()
        setNeedsUpdateOfHomeIndicatorAutoHidden()
    }

    private func applyNativeContentScale(to view: UIView) {
        let screen = view.window?.screen ?? UIScreen.main
        let scale = max(screen.nativeScale, screen.scale, 1.0)
        view.contentScaleFactor = scale
        view.layer.contentsScale = scale
        for subview in view.subviews {
            applyNativeContentScale(to: subview)
        }
    }
}


// ARMSX2_ALUNE_STYLE_HOME_BAR_V3_BEGIN
// Alune does the Home/Gesture Bar part from the active emulation UIViewController itself:
// its emulation controller directly overrides prefersHomeIndicatorAutoHidden.
// ARMSX2 is SwiftUI-driven, so this root container presents GameScreenView inside a
// dedicated UIKit controller when AppState.currentScreen == .playing.
@MainActor
private final class ARMSX2GameplayHostingController<Content: View>: UIHostingController<Content> {
    private var refreshTimer: Timer?

    override var prefersStatusBarHidden: Bool {
        AppState.shared.hideStatusBar
    }

    override var prefersHomeIndicatorAutoHidden: Bool {
        true
    }

    override var preferredScreenEdgesDeferringSystemGestures: UIRectEdge {
        .all
    }

    override var childForHomeIndicatorAutoHidden: UIViewController? {
        nil
    }

    override var childForScreenEdgesDeferringSystemGestures: UIViewController? {
        nil
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        modalPresentationCapturesStatusBarAppearance = true
        view.backgroundColor = .black
        view.isOpaque = true
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        refreshSystemGestureChrome()
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        refreshSystemGestureChrome()
        // iOS may briefly re-evaluate the Home Indicator after SwiftUI/Metal view changes
        // or after the app becomes active. Keep nudging UIKit while the gameplay controller
        // is the active full-screen controller, like a game view controller would do.
        refreshTimer?.invalidate()
        let timer = Timer(timeInterval: 0.75, repeats: true) { [weak self] _ in
            self?.refreshSystemGestureChrome()
        }
        refreshTimer = timer
        RunLoop.main.add(timer, forMode: .common)
    }

    override func viewWillDisappear(_ animated: Bool) {
        super.viewWillDisappear(animated)
        refreshTimer?.invalidate()
        refreshTimer = nil
    }

    @objc func refreshSystemGestureChrome() {
        setNeedsStatusBarAppearanceUpdate()
        setNeedsUpdateOfHomeIndicatorAutoHidden()
        setNeedsUpdateOfScreenEdgesDeferringSystemGestures()
    }
}

@MainActor
private final class ARMSX2RootContainerController: UIViewController {
    private let menuController = ARMSX2HostingController(rootView: RootView())
    private var gameplayController: ARMSX2GameplayHostingController<GameScreenView>?

    override var prefersStatusBarHidden: Bool {
        AppState.shared.hideStatusBar
    }

    override var prefersHomeIndicatorAutoHidden: Bool {
        AppState.shared.currentScreen == .playing || AppState.shared.hideHomeIndicator
    }

    override var preferredScreenEdgesDeferringSystemGestures: UIRectEdge {
        (AppState.shared.currentScreen == .playing || AppState.shared.hideHomeIndicator) ? .all : []
    }

    override var childForHomeIndicatorAutoHidden: UIViewController? {
        gameplayController ?? presentedViewController ?? menuController
    }

    override var childForScreenEdgesDeferringSystemGestures: UIViewController? {
        gameplayController ?? presentedViewController ?? menuController
    }

    override var childForStatusBarHidden: UIViewController? {
        gameplayController ?? presentedViewController ?? menuController
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = .clear
        embedMenuController()

        NotificationCenter.default.addObserver(
            self,
            selector: #selector(emulationPresentationNeedsUpdate),
            name: AppState.emulationPresentationNeedsUpdateNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(systemChromeNeedsUpdate),
            name: AppState.systemChromeNeedsUpdateNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(systemChromeNeedsUpdate),
            name: UIApplication.didBecomeActiveNotification,
            object: nil
        )
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        updateGameplayPresentation(animated: false)
        refreshSystemChrome()
    }

    private func embedMenuController() {
        addChild(menuController)
        menuController.view.translatesAutoresizingMaskIntoConstraints = false
        menuController.view.backgroundColor = .clear
        menuController.view.isOpaque = false
        view.addSubview(menuController.view)
        NSLayoutConstraint.activate([
            menuController.view.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            menuController.view.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            menuController.view.topAnchor.constraint(equalTo: view.topAnchor),
            menuController.view.bottomAnchor.constraint(equalTo: view.bottomAnchor),
        ])
        menuController.didMove(toParent: self)
    }

    @objc private func emulationPresentationNeedsUpdate() {
        updateGameplayPresentation(animated: false)
        refreshSystemChrome()
    }

    @objc private func systemChromeNeedsUpdate() {
        refreshSystemChrome()
    }

    private func updateGameplayPresentation(animated: Bool) {
        let shouldShowGameplay = AppState.shared.currentScreen == .playing

        if shouldShowGameplay {
            if gameplayController == nil {
                let controller = ARMSX2GameplayHostingController(rootView: GameScreenView())
                controller.modalPresentationStyle = .fullScreen
                controller.modalTransitionStyle = .crossDissolve
                controller.modalPresentationCapturesStatusBarAppearance = true
                gameplayController = controller
                present(controller, animated: animated) { [weak self, weak controller] in
                    controller?.refreshSystemGestureChrome()
                    self?.refreshSystemChrome()
                }
            } else {
                gameplayController?.refreshSystemGestureChrome()
            }
        } else if let controller = gameplayController {
            gameplayController = nil
            controller.dismiss(animated: animated) { [weak self] in
                self?.refreshSystemChrome()
            }
        }
    }

    private func refreshSystemChrome() {
        setNeedsStatusBarAppearanceUpdate()
        setNeedsUpdateOfHomeIndicatorAutoHidden()
        setNeedsUpdateOfScreenEdgesDeferringSystemGestures()
        gameplayController?.refreshSystemGestureChrome()
        DispatchQueue.main.async { [weak self] in
            self?.setNeedsStatusBarAppearanceUpdate()
            self?.setNeedsUpdateOfHomeIndicatorAutoHidden()
            self?.setNeedsUpdateOfScreenEdgesDeferringSystemGestures()
            self?.gameplayController?.refreshSystemGestureChrome()
        }
    }
}
// ARMSX2_ALUNE_STYLE_HOME_BAR_V3_END

@objc public class SwiftUIHost: NSObject {
    @MainActor
    @objc public static func createMenuController() -> UIViewController {
        // ARMSX2_ALUNE_STYLE_HOME_BAR_V3
        // Return a UIKit root container. The menu remains SwiftUI, but gameplay/BIOS is
        // presented by a dedicated full-screen UIViewController, matching Alune's model.
        return ARMSX2RootContainerController()
    }
}
