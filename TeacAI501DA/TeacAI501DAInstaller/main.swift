// === Claude origin ===
// Created/placed by Anthropic Claude Code at: 2026-07-02-212800
// Minimal container app that installs/updates the TeacAI501DA dext as a SystemExtension.
// Simply running it submits an OSSystemExtensionRequest.activationRequest.
// ====================

import Foundation
import SystemExtensions

let extensionID = "jp.hogehoge.TeacAI501DA"

final class RequestDelegate: NSObject, OSSystemExtensionRequestDelegate {
    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        print("[installer] replacing \(existing.bundleShortVersion)(\(existing.bundleVersion)) -> \(ext.bundleShortVersion)(\(ext.bundleVersion))")
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        print("[installer] NEEDS USER APPROVAL: open System Settings > Privacy & Security and click Allow")
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        switch result {
        case .completed:
            print("[installer] RESULT: completed (dext activated)")
        case .willCompleteAfterReboot:
        print("[installer] RESULT: willCompleteAfterReboot (activates after reboot)")
        @unknown default:
            print("[installer] RESULT: unknown (\(result.rawValue))")
        }
        exit(0)
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        print("[installer] FAILED: \(error)")
        exit(1)
    }
}

let delegate = RequestDelegate()
let request = OSSystemExtensionRequest.activationRequest(
    forExtensionWithIdentifier: extensionID, queue: .main)
request.delegate = delegate
OSSystemExtensionManager.shared.submitRequest(request)
print("[installer] activation request submitted: \(extensionID)")
RunLoop.main.run()
