// === Claude origin ===
// Created/placed by Anthropic Claude Code at: 2026-07-02-212800
// Container app that installs, updates and removes the TeacAI501DA dext.
// Launched with no arguments it asks which action to take; --install and
// --uninstall skip the prompt for scripted use.
// ====================

import AppKit
import Foundation
import SystemExtensions

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
    alert.messageText = "TEAC AI-501DA Driver"
    alert.informativeText = message
    alert.addButton(withTitle: "OK")
    NSApp.activate(ignoringOtherApps: true)
    alert.runModal()
}

func askForAction() -> Action? {
    let alert = NSAlert()
    alert.messageText = "TEAC AI-501DA Driver"
    alert.informativeText = """
        インストール / 更新: ドライバを有効にします。初回はシステム設定での承認が必要です。
        アンインストール: ドライバを削除します。管理者パスワードを求められます。

        Install / Update: activates the driver. The first install needs approval \
        in System Settings.
        Uninstall: removes the driver. You will be asked for an administrator password.
        """
    alert.addButton(withTitle: "インストール / 更新")
    alert.addButton(withTitle: "アンインストール")
    alert.addButton(withTitle: "キャンセル")
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
        report("システム設定 > 一般 > ログイン項目と機能拡張 > ドライバ機能拡張 で承認してください。\n\n"
             + "Approve under System Settings > General > Login Items & Extensions > Driver Extensions.")
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        let done = action == .install ? "ドライバを有効にしました。" : "ドライバを削除しました。"
        let doneEN = action == .install ? "The driver is active." : "The driver has been removed."
        switch result {
        case .completed:
            var text = "\(done)\n\(doneEN)"
            if action == .install {
                text += "\n\nAI-501DA の電源を入れ直すと認識されます。"
                     + "\nPower-cycle the AI-501DA to let it be picked up."
            } else {
                text += "\n\nこのアプリはゴミ箱に入れて構いません。"
                     + "\nYou can move this app to the Trash."
            }
            report(text)
        case .willCompleteAfterReboot:
            report("再起動後に完了します。\nThis completes after you restart the Mac.")
        @unknown default:
            report("RESULT: unknown (\(result.rawValue))")
        }
        exit(0)
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        report("失敗しました / Failed:\n\(error.localizedDescription)", style: .warning)
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
