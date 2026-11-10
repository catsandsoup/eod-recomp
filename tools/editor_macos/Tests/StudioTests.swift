import Foundation
import XCTest
@testable import EODModdingSuite

final class StudioTests: XCTestCase {
    @MainActor
    func testRealBackendAndProductionEditSaveReopenHandlers() async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let suite = "StudioTests." + UUID().uuidString
        let prefs = UserDefaults(suiteName: suite)!
        defer { prefs.removePersistentDomain(forName: suite) }
        let fixture = Process()
        fixture.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        fixture.arguments = ["-c", """
        import sys,json,contextlib,io
        from pathlib import Path
        sys.path.insert(0,sys.argv[1])
        from valt import compile_binary_valt
        from rst0 import pack_archive
        root=Path(sys.argv[2]); stage=root/'stage';stage.mkdir()
        (root/'disc/DATA/CAR').mkdir(parents=True)
        table=dict(tool_vtable=0,unk1=0,unk2=0,sections=[dict(name='Chassis',parent='',sec_vtable=0,tool_ptr=0,pairs=[('mass','2800')])])
        (stage/'physics.val').write_bytes(compile_binary_valt(table))
        (stage/'manifest.json').write_text(json.dumps(dict(version=6,entries=[dict(name='physics.val',fourcc='VALT',version=11,is_extractable=False)])))
        with contextlib.redirect_stdout(io.StringIO()):pack_archive(str(stage),str(root/'disc/DATA/CAR/ALGAR.CAR'))
        """, RuntimePaths.repository.appendingPathComponent("tools").path, directory.path]
        try fixture.run(); fixture.waitUntilExit()
        XCTAssertEqual(fixture.terminationStatus, 0)
        let state = StudioState(preferences: prefs)
        state.disc = directory.appendingPathComponent("disc").path
        state.project = directory.appendingPathComponent("mod/project.eodproject").path
        await state.refresh()
        XCTAssertEqual(state.assetCount, 1)
        state.selectArchive("DATA/CAR/ALGAR.CAR")
        try await settle(state)
        XCTAssertNil(state.error)
        XCTAssertEqual(state.values["0:0"], "2800")
        state.edit("0:0", "4200")
        XCTAssertTrue(state.dirty)
        state.undo(); XCTAssertEqual(state.values["0:0"], "2800")
        state.redo(); XCTAssertEqual(state.values["0:0"], "4200")
        state.save(); try await settle(state)
        XCTAssertNil(state.error)
        XCTAssertFalse(state.dirty)
        XCTAssertTrue(state.canTest)
        let saved = try Data(contentsOf: URL(fileURLWithPath: state.project))
        let reopened = StudioState(preferences: prefs)
        await reopened.loadProject(state.project)
        XCTAssertNil(reopened.error)
        XCTAssertEqual(reopened.values["0:0"], "4200")
        reopened.edit("0:0", "nan")
        reopened.save(); try await settle(reopened)
        XCTAssertNotNil(reopened.error)
        XCTAssertTrue(reopened.dirty)
        XCTAssertEqual(try Data(contentsOf: URL(fileURLWithPath: state.project)), saved)
        let damagedProject = directory.appendingPathComponent("bad.eodproject")
        var document = try JSONSerialization.jsonObject(with: saved) as! [String: Any]
        document["disc"] = directory.deletingLastPathComponent().appendingPathComponent(UUID().uuidString + "-missing-disc").path
        try JSONSerialization.data(withJSONObject: document).write(to: damagedProject)
        await reopened.loadProject(damagedProject.path)
        XCTAssertNotNil(reopened.error)
        XCTAssertEqual(reopened.project, state.project, "A failed Open must preserve the current project and edits")
        XCTAssertEqual(reopened.values["0:0"], "nan")
    }

    @MainActor
    func testTrackSkyProductionPreviewSaveAndReopen() async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let suite = "TrackTests." + UUID().uuidString
        let prefs = UserDefaults(suiteName: suite)!
        defer { prefs.removePersistentDomain(forName: suite) }
        let fixture = Process()
        fixture.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        fixture.arguments = ["-c", """
        import sys,json,contextlib,io
        from pathlib import Path
        sys.path.insert(0,sys.argv[1])
        from tex import compile_tex
        from rst0 import pack_archive
        root=Path(sys.argv[2]);stage=root/'stage';stage.mkdir()
        (root/'disc/DATA/TRACKS').mkdir(parents=True)
        entries=[]
        for name in ['sky01A.tex','sky01B.tex']:
            (stage/name).write_bytes(compile_tex(128,128,bytes((20,30,40,255))*(128*128)))
            entries.append(dict(name=name,fourcc='TEX ',version=10,is_extractable=False))
        (stage/'manifest.json').write_text(json.dumps(dict(version=6,entries=entries)))
        with contextlib.redirect_stdout(io.StringIO()):pack_archive(str(stage),str(root/'disc/DATA/TRACKS/ANOKA.TRK'))
        """, RuntimePaths.repository.appendingPathComponent("tools").path, directory.path]
        try fixture.run(); fixture.waitUntilExit(); XCTAssertEqual(fixture.terminationStatus, 0)
        let state = StudioState(preferences: prefs)
        state.disc = directory.appendingPathComponent("disc").path
        state.project = directory.appendingPathComponent("mod/project.eodproject").path
        await state.refresh()
        state.selectArchive("DATA/TRACKS/ANOKA.TRK"); try await settle(state)
        XCTAssertNil(state.error)
        XCTAssertEqual(state.environmentDetail?.skyTextures.count, 2)
        XCTAssertEqual(state.detail?.kind, "texture")
        state.tintRed = "2"; state.tintGreen = "1"; state.tintBlue = "1"
        state.prepareTexture(allSky: true); try await settle(state)
        XCTAssertNil(state.error); XCTAssertEqual(state.pendingTextures.count, 2)
        XCTAssertTrue(state.dirty); XCTAssertTrue(state.canSave)
        state.busy = true
        state.discardTextureEdits()
        XCTAssertEqual(state.pendingTextures.count, 2, "Discard must not race a pending operation")
        state.busy = false
        state.save(); try await settle(state)
        XCTAssertNil(state.error); XCTAssertFalse(state.dirty); XCTAssertTrue(state.canTest)
        XCTAssertEqual(state.savedTestTrack, "DATA/TRACKS/ANOKA.TRK")
        let reopened = StudioState(preferences: prefs)
        await reopened.loadProject(state.project)
        XCTAssertNil(reopened.error)
        XCTAssertEqual(reopened.environmentDetail?.track, "Anoka")
        XCTAssertEqual(reopened.detail?.kind, "texture")
        XCTAssertEqual(reopened.savedTestTrack, "DATA/TRACKS/ANOKA.TRK")
        XCTAssertFalse(reopened.dirty)
    }

    @MainActor
    private func settle(_ state: StudioState) async throws {
        let deadline = Date().addingTimeInterval(10)
        while state.busy && Date() < deadline { try await Task.sleep(for: .milliseconds(20)) }
        XCTAssertFalse(state.busy, "Production operation did not finish")
    }
}
