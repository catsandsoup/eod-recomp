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
        state.begin(.write)
        state.discardTextureEdits()
        XCTAssertEqual(state.pendingTextures.count, 2, "Discard must not race a pending write")
        state.end(.write)
        state.save(); try await settle(state)
        XCTAssertNil(state.error); XCTAssertFalse(state.dirty); XCTAssertTrue(state.canTest)
        XCTAssertEqual(state.savedTestTrack, "DATA/TRACKS/ANOKA.TRK")
        let reopened = StudioState(preferences: prefs)
        await reopened.startup()
        XCTAssertNil(reopened.error)
        XCTAssertEqual(reopened.environmentDetail?.track, "Anoka")
        XCTAssertEqual(reopened.detail?.kind, "texture")
        XCTAssertEqual(reopened.savedTestTrack, "DATA/TRACKS/ANOKA.TRK")
        XCTAssertFalse(reopened.dirty)
    }


    @MainActor
    func testTexturePipelineReplaceExportPackageInstallAndBusyAccounting() async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let suite = "PipelineTests." + UUID().uuidString
        let prefs = UserDefaults(suiteName: suite)!
        defer { prefs.removePersistentDomain(forName: suite) }
        let fixture = Process()
        fixture.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        fixture.arguments = ["-c", """
        import sys,json,contextlib,io,random
        from pathlib import Path
        sys.path.insert(0,sys.argv[1])
        from tex import compile_tex, write_png
        from rst0 import pack_archive
        root=Path(sys.argv[2]);stage=root/'stage';stage.mkdir()
        (root/'disc/DATA/TRACKS').mkdir(parents=True)
        (root/'disc/SLUS_209.10').write_bytes(b'synthetic executable')
        rng=random.Random(5)
        palette=[bytes(rng.randrange(256) for _ in range(3))+b'\\xff' for _ in range(8)]
        entries=[]
        for name in ['sky01A.tex','sky01B.tex']:
            (stage/name).write_bytes(compile_tex(16,16,b''.join(rng.choice(palette) for _ in range(256))))
            entries.append(dict(name=name,fourcc='TEX ',version=10,is_extractable=False))
        (stage/'manifest.json').write_text(json.dumps(dict(version=6,entries=entries)))
        with contextlib.redirect_stdout(io.StringIO()):pack_archive(str(stage),str(root/'disc/DATA/TRACKS/ANOKA.TRK'))
        for size in (16,32,48):
            write_png(str(root/f'{size}.png'),size,size,bytes(rng.randrange(256) for _ in range(size*size*4)))
        """, RuntimePaths.repository.appendingPathComponent("tools").path, directory.path]
        try fixture.run(); fixture.waitUntilExit(); XCTAssertEqual(fixture.terminationStatus, 0)
        let state = StudioState(preferences: prefs)
        state.disc = directory.appendingPathComponent("disc").path
        state.project = directory.appendingPathComponent("mod/project.eodproject").path
        state.profileRoot = directory.appendingPathComponent("profiles").path
        state.jobsRoot = directory.appendingPathComponent("jobs").path
        await state.refresh()
        state.selectArchive("DATA/TRACKS/ANOKA.TRK"); try await settle(state)
        XCTAssertNil(state.error)

        state.begin(); state.begin(); state.end()
        XCTAssertTrue(state.busy, "Busy must not clear while another operation is still running")
        state.end(); XCTAssertFalse(state.busy)

        await state.checkReplacement(directory.appendingPathComponent("48.png"))
        XCTAssertNil(state.replacement); XCTAssertNotNil(state.error)
        state.error = nil
        await state.checkReplacement(directory.appendingPathComponent("32.png"))
        XCTAssertEqual(state.replacement?.check.mode, "host"); XCTAssertEqual(state.replacement?.check.scale, 2)
        await state.applyReplacement()
        XCTAssertNil(state.error); XCTAssertNil(state.replacement)
        XCTAssertEqual(state.currentHost?.k, 2)
        XCTAssertFalse(state.dirty, "A recorded host replacement is persisted, not pending")
        let saved = try JSONSerialization.jsonObject(with: Data(contentsOf: URL(fileURLWithPath: state.project))) as! [String: Any]
        XCTAssertEqual((saved["hostTextures"] as? [String: Any])?.count, 1)
        XCTAssertEqual(saved["version"] as? Int, 1)

        await state.checkReplacement(directory.appendingPathComponent("16.png"))
        XCTAssertEqual(state.replacement?.check.mode, "native")
        await state.applyReplacement(); try await settle(state)
        XCTAssertEqual(state.pendingTextures.count, 1); XCTAssertTrue(state.dirty)
        await state.buildPackage()
        XCTAssertNil(state.builtPackage, "Packaging must not run over unsaved edits")
        state.save(); try await settle(state)
        XCTAssertNil(state.error)

        await state.buildPackage()
        XCTAssertNil(state.error)
        XCTAssertEqual(state.builtPackage?.host, 1); XCTAssertEqual(state.builtPackage?.native, 1)
        await state.installPackage()
        XCTAssertNil(state.error)
        XCTAssertEqual(state.profile?.packages.count, 1); XCTAssertTrue(state.profileActive); XCTAssertTrue(state.canTest)
        let id = state.profile!.packages[0].id
        await state.profileOperation("profile_disable", id: id)
        XCTAssertFalse(state.profileActive)
        await state.profileOperation("profile_enable", id: id)
        XCTAssertTrue(state.profileActive)
        await state.profileOperation("profile_enable", id: "missing")
        XCTAssertNotNil(state.error); XCTAssertTrue(state.profileActive, "A failed operation leaves the profile as it was")
        state.error = nil

        let reopened = StudioState(preferences: prefs)
        reopened.profileRoot = state.profileRoot
        await reopened.loadProject(state.project)
        XCTAssertEqual(reopened.hostTextures.count, 1)
        XCTAssertTrue(reopened.profileActive)

        let destination = directory.appendingPathComponent("textures-out").path
        await state.startExport(to: destination)
        XCTAssertNil(state.error)
        XCTAssertEqual(state.exportStatus?.state, "complete"); XCTAssertEqual(state.exportStatus?.exported, 2)
        XCTAssertFalse(state.busy)
        await state.startExport(to: destination)
        XCTAssertNil(state.error, "Exporting into a finished export folder resumes it")
        XCTAssertEqual(state.exportStatus?.exported, 2)
        await state.startExport(to: state.disc + "/DATA/out")
        XCTAssertNotNil(state.error); XCTAssertFalse(state.busy)

        await state.profileOperation("profile_uninstall", id: id)
        XCTAssertFalse(state.profileActive); XCTAssertEqual(state.profile?.packages.count, 0)
    }

    @MainActor
    private func settle(_ state: StudioState) async throws {
        let deadline = Date().addingTimeInterval(10)
        while (state.loading || state.isWriting) && Date() < deadline { try await Task.sleep(for: .milliseconds(20)) }
        XCTAssertFalse(state.loading || state.isWriting, "Production operation did not finish")
    }

    /// The owner's concurrency rule: a long export must not block selecting or inspecting assets.
    @MainActor
    func testCanInspectWhileExportRunning() async throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let suite = "ConcurrencyTests." + UUID().uuidString
        let prefs = UserDefaults(suiteName: suite)!
        defer { prefs.removePersistentDomain(forName: suite) }
        let fixture = Process()
        fixture.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        fixture.arguments = ["-c", """
        import sys,json,contextlib,io,random
        from pathlib import Path
        sys.path.insert(0,sys.argv[1])
        from tex import compile_tex
        from rst0 import pack_archive
        root=Path(sys.argv[2]);stage=root/'stage';stage.mkdir()
        (root/'disc/DATA/TRACKS').mkdir(parents=True)
        (root/'disc/SLUS_209.10').write_bytes(b'synthetic executable')
        rng=random.Random(7)
        entries=[]
        for i in range(80):
            palette=[bytes(rng.randrange(256) for _ in range(3))+b'\\xff' for _ in range(8)]
            (stage/f't{i:03d}.tex').write_bytes(compile_tex(64,64,b''.join(rng.choice(palette) for _ in range(64*64))))
            entries.append(dict(name=f't{i:03d}.tex',fourcc='TEX ',version=10,is_extractable=False))
        (stage/'manifest.json').write_text(json.dumps(dict(version=6,entries=entries)))
        with contextlib.redirect_stdout(io.StringIO()):pack_archive(str(stage),str(root/'disc/DATA/TRACKS/ANOKA.TRK'))
        """, RuntimePaths.repository.appendingPathComponent("tools").path, directory.path]
        try fixture.run(); fixture.waitUntilExit(); XCTAssertEqual(fixture.terminationStatus, 0)
        let state = StudioState(preferences: prefs)
        state.disc = directory.appendingPathComponent("disc").path
        state.project = directory.appendingPathComponent("mod/project.eodproject").path
        state.profileRoot = directory.appendingPathComponent("profiles").path
        state.jobsRoot = directory.appendingPathComponent("jobs").path
        await state.refresh()
        state.selectArchive("DATA/TRACKS/ANOKA.TRK"); try await settle(state)
        XCTAssertNil(state.error)

        let exporting = Task { await state.startExport(to: directory.appendingPathComponent("out").path) }
        let started = Date().addingTimeInterval(10)
        while !state.exportRunning && Date() < started { try await Task.sleep(for: .milliseconds(5)) }
        XCTAssertTrue(state.exportRunning, "export did not start")
        XCTAssertFalse(state.busy, "an export must not raise the app-wide write gate")
        for name in ["t040.tex", "t041.tex", "t042.tex"] {
            let clicked = Date()
            state.selectAsset(name)
            while state.detail?.name != name && Date().timeIntervalSince(clicked) < 10 { try await Task.sleep(for: .milliseconds(5)) }
            XCTAssertEqual(state.detail?.name, name, "inspect must complete while the export runs")
            XCTAssertNil(state.error)
        }
        XCTAssertTrue(state.isRunning(.export), "the export finished before the inspects; the check proved nothing")
        await exporting.value
        XCTAssertEqual(state.exportStatus?.state, "complete")
        XCTAssertEqual(state.exportStatus?.exported, 80)
    }
}
