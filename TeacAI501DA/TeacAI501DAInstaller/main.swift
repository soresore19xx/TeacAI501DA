// === Claude origin ===
// Created/placed by Anthropic Claude Code at: 2026-07-02-212800
// Container app that installs, updates and removes the TeacAI501DA dext.
// Launched with no arguments it asks which action to take; --install and
// --uninstall skip the prompt for scripted use.
// ====================

import AppKit
import Foundation
import SystemExtensions

// Line-buffer stdout. Swift block-buffers it when it is not a terminal, so a
// scripted or LaunchAgent run would otherwise show nothing until the process
// exits — exactly when the progress messages stop being useful.
setvbuf(stdout, nil, _IOLBF, 0)

let extensionID = "jp.hogehoge.TeacAI501DA"

enum Action {
    case install
    case uninstall
}

/// Whether the user drove this run from a dialog. Scripted runs stay silent
/// and report through stdout only.
var interactive = false

func report(_ message: String, style: NSAlert.Style = .informational) {
    print("[installer] \(message)")
    guard interactive else { return }
    let alert = NSAlert()
    alert.alertStyle = style
    alert.messageText = "Unofficial TEAC USB DAC Driver"
    alert.informativeText = message
    alert.addButton(withTitle: "OK")
    NSApp.activate(ignoringOtherApps: true)
    alert.runModal()
}

func askForAction() -> Action? {
    let alert = NSAlert()
    alert.messageText = "Unofficial TEAC USB DAC Driver"
    alert.informativeText = """
        Install / Update activates the driver. The first install needs approval \
        in System Settings.

        Uninstall removes the driver. You will be asked for an administrator \
        password.
        """
    alert.addButton(withTitle: "Install / Update")
    alert.addButton(withTitle: "Uninstall")
    alert.addButton(withTitle: "Cancel")
    NSApp.activate(ignoringOtherApps: true)
    switch alert.runModal() {
    case .alertFirstButtonReturn:  return .install
    case .alertSecondButtonReturn: return .uninstall
    default:                       return nil
    }
}

final class RequestDelegate: NSObject, OSSystemExtensionRequestDelegate {
    let action: Action

    init(action: Action) {
        self.action = action
    }

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        print("[installer] replacing \(existing.bundleShortVersion)(\(existing.bundleVersion)) -> \(ext.bundleShortVersion)(\(ext.bundleVersion))")
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        report("Approve the driver under System Settings > General > "
             + "Login Items & Extensions > Driver Extensions.")
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        switch result {
        case .completed:
            var text = action == .install ? "The driver is active."
                                          : "The driver has been removed."
            text += action == .install
                ? "\n\nPower-cycle the DAC (AI-501DA or UD-501) so it re-enumerates."
                : "\n\nYou can move this app to the Trash."
            report(text)
        case .willCompleteAfterReboot:
            report("This completes after you restart the Mac.")
        @unknown default:
            report("RESULT: unknown (\(result.rawValue))")
        }
        exit(0)
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        report("Failed: \(error.localizedDescription)", style: .warning)
        exit(1)
    }
}

let args = CommandLine.arguments.dropFirst()
let action: Action
if args.contains("--uninstall") {
    action = .uninstall
} else if args.contains("--install") {
    action = .install
} else {
    interactive = true
    NSApplication.shared.setActivationPolicy(.regular)
    guard let chosen = askForAction() else {
        print("[installer] cancelled")
        exit(0)
    }
    action = chosen
}

let delegate = RequestDelegate(action: action)
let request: OSSystemExtensionRequest
switch action {
case .install:
    request = OSSystemExtensionRequest.activationRequest(
        forExtensionWithIdentifier: extensionID, queue: .main)
    print("[installer] activation request submitted: \(extensionID)")
case .uninstall:
    request = OSSystemExtensionRequest.deactivationRequest(
        forExtensionWithIdentifier: extensionID, queue: .main)
    print("[installer] deactivation request submitted: \(extensionID)")
}
request.delegate = delegate
OSSystemExtensionManager.shared.submitRequest(request)
RunLoop.main.run()
