import AppKit
import SwiftUI

#if DEBUG || EOD_KIT_MEASURE
/// Debug builds only: `EOD_KIT_OPEN="DATA/TRACKS/ANOKA.TRK#oval.ild"` selects that asset once the catalog
/// has loaded, so a screenshot of a specific view can be taken without clicking. With
/// `EOD_KIT_MEASURE=1` the longest main-thread stall in the 4 seconds after selection is printed to stderr.
extension MainView {
    @MainActor func debugOpenAtLaunch() async {
        let environment = ProcessInfo.processInfo.environment
        guard let list = environment["EOD_KIT_OPEN"] else { return }
        if let window = NSApp.windows.first { window.setContentSize(NSSize(width: 1500, height: 1500)) }
        func settle() async {
            for _ in 0..<600 { try? await Task.sleep(nanoseconds: 50_000_000); if !state.loading { break } }
        }
        for _ in 0..<600 {
            try? await Task.sleep(nanoseconds: 50_000_000)
            if !state.archives.isEmpty && !state.loading { break }
        }
        // EOD_KIT_EXPORT_TO=<folder> starts "Export all textures" first, to test responsiveness during an export.
        if let folder = environment["EOD_KIT_EXPORT_TO"] {
            Task { await state.startExport(to: folder) }
            try? await Task.sleep(nanoseconds: 3_000_000_000)
        }
        let measure = environment["EOD_KIT_MEASURE"] != nil
        // Several targets can be given separated by ";" and are opened one after another.
        for target in list.split(separator: ";").map(String.init) {
            guard let split = target.firstIndex(of: "#") else { continue }
            let archiveName = String(target[..<split]), assetName = String(target[target.index(after: split)...])
            guard let item = state.archives.first(where: { $0.path.caseInsensitiveCompare(archiveName) == .orderedSame }) else {
                FileHandle.standardError.write(Data("EOD_KIT_OPEN: archive \(archiveName) not found\n".utf8)); return
            }
            if state.archive != item.path { state.selectArchive(item.path); await settle() }
            let monitor = StallMonitor()
            if measure { monitor.start() }
            let begun = ContinuousClock.now
            state.selectAsset(assetName)
            while state.detail == nil || state.loading { try? await Task.sleep(nanoseconds: 5_000_000) }
            let loaded = ContinuousClock.now - begun
            try? await Task.sleep(nanoseconds: list.contains(";") ? 2_500_000_000 : 4_000_000_000)
            if measure {
                FileHandle.standardError.write(Data("EOD_KIT_MEASURE \(assetName): detail loaded in \(loaded), longest main-thread stall \(monitor.longest) ms, export \(state.exportStatus?.state ?? "off") \(state.exportStatus?.done ?? 0)/\(state.exportStatus?.total ?? 0)\n".utf8))
            }
            monitor.stop()
        }
    }
}

@MainActor private final class StallMonitor {
    private(set) var longest = 0
    private var last = ContinuousClock.now
    private var timer: Timer?
    func stop() { timer?.invalidate(); timer = nil }
    func start() {
        last = .now
        timer = Timer.scheduledTimer(withTimeInterval: 0.01, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated {
                guard let self else { return }
                let now = ContinuousClock.now
                let gap = (now - self.last).components
                self.longest = max(self.longest, Int(gap.seconds * 1000 + gap.attoseconds / 1_000_000_000_000_000))
                self.last = now
            }
        }
    }
}
#endif
