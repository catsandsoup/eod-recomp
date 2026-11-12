import AppKit
import SwiftUI

#if DEBUG || EOD_KIT_MEASURE
/// Debug builds only: `EOD_KIT_OPEN="DATA/TRACKS/ANOKA.TRK#oval.ild"` selects that asset once the catalog
/// has loaded, so a screenshot of a specific view can be taken without clicking. With
/// `EOD_KIT_MEASURE=1` the longest main-thread stall in the 4 seconds after selection is printed to stderr.
extension MainView {
    @MainActor func debugOpenAtLaunch() async {
        guard let target = ProcessInfo.processInfo.environment["EOD_KIT_OPEN"], let split = target.firstIndex(of: "#") else { return }
        let archiveName = String(target[..<split]), assetName = String(target[target.index(after: split)...])
        func settle() async {
            for _ in 0..<600 { try? await Task.sleep(nanoseconds: 50_000_000); if !state.busy { break } }
        }
        for _ in 0..<600 {
            try? await Task.sleep(nanoseconds: 50_000_000)
            if !state.archives.isEmpty && !state.busy { break }
        }
        guard let item = state.archives.first(where: { $0.path.caseInsensitiveCompare(archiveName) == .orderedSame }) else {
            FileHandle.standardError.write(Data("EOD_KIT_OPEN: archive \(archiveName) not found\n".utf8)); return
        }
        state.selectArchive(item.path)
        await settle()
        let measure = ProcessInfo.processInfo.environment["EOD_KIT_MEASURE"] != nil
        let monitor = StallMonitor()
        if measure { monitor.start() }
        let begun = ContinuousClock.now
        state.selectAsset(assetName)
        while state.detail == nil || state.busy { try? await Task.sleep(nanoseconds: 5_000_000) }
        let loaded = ContinuousClock.now - begun
        try? await Task.sleep(nanoseconds: 4_000_000_000)
        if measure {
            FileHandle.standardError.write(Data("EOD_KIT_MEASURE \(assetName): detail loaded in \(loaded), longest main-thread stall \(monitor.longest) ms\n".utf8))
        }
    }
}

@MainActor private final class StallMonitor {
    private(set) var longest = 0
    private var last = ContinuousClock.now
    private var timer: Timer?
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
