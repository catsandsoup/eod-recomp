import AppKit
import SceneKit
import SwiftUI

struct MainView: View {
    @ObservedObject var state: StudioState
    @State private var wireframe = false
    var body: some View {
        content
        #if DEBUG || EOD_KIT_MEASURE
            .task { await debugOpenAtLaunch() }
        #endif
    }

    private var content: some View {
        VStack(spacing: 0) {
            HStack {
                VStack(alignment: .leading, spacing: 3) {
                    Text("EVE OF DESTRUCTION").font(.system(size: 10, weight: .bold, design: .rounded)).tracking(2).foregroundStyle(.secondary)
                    Text("Creation Kit").font(.system(size: 19, weight: .semibold))
                }
                Divider().frame(height: 28).padding(.horizontal, 8)
                VStack(alignment: .leading, spacing: 4) {
                    Text(URL(fileURLWithPath: state.project).deletingLastPathComponent().lastPathComponent).font(.headline)
                    Text(state.editingAsset.isEmpty ? "Project" : "\(state.editingAsset)\(state.dirty ? " · Unsaved" : "")")
                        .font(.caption).foregroundStyle(state.dirty ? .orange : .secondary)
                }
                Spacer()
                Button("Open Project…") { state.openProject() }
                Button("Game Folder…") { state.chooseDisc() }
                Button("Export All Textures…") { state.exportAllTextures() }
                Button("Save & Build") { state.save() }.disabled(!state.canSave)
                if state.gameRunning {
                    Button("Stop Test") { state.stopGame() }
                } else {
                    Button("Run Mod in Game") { state.testGame() }.disabled(!state.canTest)
                }
            }
            .padding(12).disabled(state.busy)
            Divider()
            HStack(spacing: 6) {
                ForEach(["Assets", "Mods"], id: \.self) { tab in
                    Button { state.studio = tab } label: {
                        Label(tab == "Assets" ? "Asset Workspace" : "Mods", systemImage: tab == "Assets" ? "square.stack.3d.up.fill" : "shippingbox.fill")
                            .font(.system(size: 12, weight: .medium)).padding(.horizontal, 12).padding(.vertical, 7)
                            .background(Color.blue.opacity(state.studio == tab ? 0.22 : 0), in: RoundedRectangle(cornerRadius: 5))
                    }.buttonStyle(.plain)
                }
                Spacer()
                Text(state.archive.isEmpty ? "No archive selected" : state.archive).font(.caption.monospaced()).foregroundStyle(.secondary)
                if state.detail?.kind == "model" { Toggle("Wireframe", isOn: $wireframe).toggleStyle(.checkbox).font(.caption) }
            }.padding(.horizontal, 12).padding(.vertical, 7).background(Color.black.opacity(0.15))
            Divider()
            if state.studio == "Mods" { ModsView(state: state) } else {
            HSplitView {
                VStack(alignment: .leading, spacing: 0) {
                    Label("Game Sources", systemImage: "externaldrive.fill").font(.caption.weight(.semibold)).foregroundStyle(.secondary).padding(12)
                    List(selection: Binding<String?>(get: { state.archive.isEmpty ? nil : state.archive },
                        set: { if let value = $0 { state.selectArchive(value) } })) {
                        ForEach(state.archives) { item in
                            VStack(alignment: .leading, spacing: 2) {
                                Text(item.name).font(.system(.body, design: .monospaced))
                                Text("\(item.count) assets · \(ByteCountFormatter.string(fromByteCount: Int64(item.size), countStyle: .file))")
                                    .font(.caption).foregroundStyle(.secondary)
                            }.tag(item.path).help(item.path)
                        }
                    }.disabled(state.busy || state.gameRunning)
                }.frame(minWidth: 150, idealWidth: 180, maxWidth: 240)
                VStack(alignment: .leading, spacing: 0) {
                    TextField("Find asset or type", text: $state.search).textFieldStyle(.roundedBorder).padding(10)
                    List(selection: Binding<String?>(get: { state.asset.isEmpty ? nil : state.asset },
                        set: { if let value = $0 { state.selectAsset(value) } })) {
                        ForEach(state.filteredAssets) { item in
                            VStack(alignment: .leading, spacing: 2) {
                                Text(item.name).font(.system(.body, design: .monospaced))
                                Text("\(item.type) · \(item.size.formatted()) bytes").font(.caption).foregroundStyle(.secondary)
                            }.tag(item.name)
                        }
                    }.disabled(state.busy || state.gameRunning)
                }.frame(minWidth: 150, idealWidth: 180, maxWidth: 240)
                VStack(alignment: .leading, spacing: 0) {
                    if let detail = state.detail {
                        HStack(alignment: .firstTextBaseline) {
                            Text(detail.name).font(.title2.weight(.semibold))
                            Text(detail.type).font(.caption.monospaced()).foregroundStyle(.secondary)
                            Spacer()
                            Button("Export File…") { state.exportNativeAsset() }.font(.caption).disabled(state.busy)
                        }.padding(16)
                        Divider()
                        assetContent(detail)
                        Divider()
                        Text("\(detail.size.formatted()) bytes · SHA256 \(detail.sha256.prefix(16))")
                            .font(.system(size: 10, design: .monospaced)).foregroundStyle(.secondary).textSelection(.enabled).padding(10)
                    } else {
                        Spacer()
                        VStack(alignment: .leading, spacing: 12) {
                            Text(state.asset.isEmpty ? "Game Asset Workspace" : state.asset).font(.title2)
                            Text("\(state.archives.count) archives · \(state.assetCount.formatted()) assets").foregroundStyle(.secondary)
                        }.padding(24)
                        Spacer()
                    }
                }.frame(minWidth: 350, maxWidth: .infinity, maxHeight: .infinity)
                inspector.frame(minWidth: 250, idealWidth: 280, maxWidth: 340)
            }
            }
            if let error = state.error {
                Divider()
                HStack(alignment: .top) {
                    Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                    Text(error).font(.callout).textSelection(.enabled)
                    Spacer()
                    Button("Dismiss") { state.error = nil }
                }.padding(10).background(Color.orange.opacity(0.08))
            }
            Divider()
            HStack {
                if state.exportRunning, let export = state.exportStatus {
                    ProgressView(value: Double(export.done), total: Double(max(export.total, 1))).frame(width: 220)
                    Button("Cancel Export") { Task { await state.cancelExport() } }.font(.caption)
                } else if state.busy { ProgressView().controlSize(.small) }
                Text(state.status).lineLimit(1).font(.caption).textSelection(.enabled)
                Spacer()
                if let path = state.logPath {
                    Button("Session Log") { NSWorkspace.shared.open(URL(fileURLWithPath: path)) }.font(.caption)
                }
                Button("Project Folder") { NSWorkspace.shared.selectFile(state.project, inFileViewerRootedAtPath: "") }.font(.caption)
            }.padding(.horizontal, 12).padding(.vertical, 8)
        }
    }

    @ViewBuilder private func assetContent(_ detail: AssetDetail) -> some View {
        if detail.kind == "model", let parts = detail.parts {
            VStack(alignment: .leading, spacing: 0) {
                RetailMeshView(parts: parts, identity: detail.sha256, wireframe: wireframe).frame(minHeight: 300)
                Text("\(parts.count) parts · \(parts.reduce(0) { $0 + $1.positions.count }.formatted()) vertices · \(parts.reduce(0) { $0 + $1.triangles.count }.formatted()) triangles")
                    .font(.caption).foregroundStyle(.secondary).padding(12)
                if let paint = detail.paintDescription {
                    Text(paint).font(.caption).foregroundStyle(.secondary).padding(.horizontal, 12)
                }
                let issues = Array(Set(parts.compactMap(\.textureError))).sorted()
                if !issues.isEmpty {
                    Text(issues.joined(separator: "\n") + "\nMissing textures appear gray.")
                        .font(.caption).foregroundStyle(.secondary).padding([.horizontal, .bottom], 12)
                }
            }
        } else if detail.kind == "texture", let path = detail.image, let image = NSImage(contentsOfFile: path) {
            VStack(spacing: 0) {
                HStack {
                    Button("Export PNG…") { state.exportTexture() }
                    Button("Replace with Image…") { state.replaceTexture() }.disabled(state.gameRunning || ![4, 8].contains(detail.metadata?["bits_per_pixel"] ?? 0))
                    Spacer()
                }.padding(10).disabled(state.busy)
                if let plan = state.replacement {
                    let check = plan.check
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(check.mode == "native" ? "Same size, palette reduced"
                                 : "High-resolution, \(check.scale ?? 0)× larger").font(.callout.weight(.semibold))
                            Text(check.mode == "native" ? "\(check.original.width) × \(check.original.height)"
                                 : "\(check.original.width) × \(check.original.height) → \(check.png.width) × \(check.png.height)")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button("Cancel") { state.cancelReplacement() }
                        Button("Apply") { Task { await state.applyReplacement() } }.keyboardShortcut(.defaultAction)
                    }.padding(10).background(Color.blue.opacity(0.1)).disabled(state.busy)
                } else if let host = state.currentHost {
                    HStack {
                        Text("High-resolution replacement · \(host.k)× · \(host.width) × \(host.height)").font(.caption)
                        Spacer()
                        Button("Remove") { Task { await state.removeHostReplacement() } }.font(.caption).disabled(state.busy)
                    }.padding(.horizontal, 10).padding(.bottom, 8)
                }
            ScrollView {
                Image(nsImage: image).resizable().interpolation(.none).scaledToFit().padding(20)
                if let meta = detail.metadata { Text("\(meta["width"] ?? 0) × \(meta["height"] ?? 0) · \(meta["levels"] ?? 0) mip levels").foregroundStyle(.secondary) }
            }
            }
        } else if detail.kind == "format" {
            FormatInspectorView(state: state, detail: detail)
        } else if let sections = detail.sections {
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    ForEach(sections.indices, id: \.self) { index in
                        let section = sections[index]
                        DisclosureGroup("\(section.name) · \(section.fields.count) values") {
                            VStack(spacing: 6) {
                                ForEach(section.fields.indices, id: \.self) { i in
                                    let field = section.fields[i]
                                    HStack(alignment: .top) {
                                        Text(field.key).foregroundStyle(.secondary)
                                        Spacer()
                                        if field.editable {
                                            let key = field.id
                                            TextField(field.key, text: Binding(get: { state.values[key] ?? field.value }, set: { state.edit(key, $0) }))
                                                .textFieldStyle(.roundedBorder).frame(minWidth: 130, maxWidth: 300)
                                        } else { Text(field.value).multilineTextAlignment(.trailing) }
                                    }.font(.system(.caption, design: .monospaced)).textSelection(.enabled)
                                }
                            }.padding(.top, 6)
                        }
                    }
                }.padding(18)
            }.disabled(state.busy || state.gameRunning)
        } else {
            Text("This file can't be shown.").foregroundStyle(.secondary).padding(20)
        }
    }

    private var inspector: some View {
        VStack(alignment: .leading, spacing: 0) {
            Label("Inspector", systemImage: "slider.horizontal.3").font(.caption.weight(.semibold))
                .foregroundStyle(.secondary).padding(12)
            Divider()
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    if let environment = state.environmentDetail {
                        Text(environment.track).font(.title3.weight(.semibold))
                        Text("Track Environment").font(.caption).foregroundStyle(.secondary)
                        Text("\(environment.skyTextures.count) sky tiles · \(environment.textures.count) textures").font(.caption)
                        Button("Show Sky Textures") { state.search = "sky" }
                        Divider()
                        Text("Sky tint (red, green, blue multipliers)").font(.caption.weight(.semibold))
                        HStack {
                            TextField("Red", text: $state.tintRed)
                            TextField("Green", text: $state.tintGreen)
                            TextField("Blue", text: $state.tintBlue)
                        }.textFieldStyle(.roundedBorder)
                        Button("Preview Whole Sky Tint") { state.prepareTexture(allSky: true) }
                            .disabled(environment.skyTextures.isEmpty)
                        if !state.pendingTextures.isEmpty {
                            Text("\(state.pendingTextures.count) textures changed").font(.caption)
                            Button("Discard Texture Edits") { state.discardTextureEdits() }.disabled(state.busy || state.gameRunning)
                        }
                        Text("Replacements keep each texture's original size and color count.")
                            .font(.caption).foregroundStyle(.secondary)
                        if let path = state.nativePreview, let image = NSImage(contentsOfFile: path) {
                            Divider()
                            Text("Test race capture").font(.caption.weight(.semibold))
                            Image(nsImage: image).resizable().scaledToFit()
                        }
                    } else if let physics = state.physicsDetail, state.editingAsset.lowercased() == "physics.val" {
                        Text(URL(fileURLWithPath: state.archive).deletingPathExtension().lastPathComponent).font(.title3.weight(.semibold))
                        Text("Vehicle Physics").font(.caption).foregroundStyle(.secondary)
                        ForEach((physics.sections ?? []).indices, id: \.self) { index in
                            let section = physics.sections![index]
                            let fields = section.fields.filter { $0.featured == true }
                            if !fields.isEmpty {
                                Divider()
                                Text(section.name).font(.subheadline.weight(.semibold)).foregroundStyle(.secondary)
                                ForEach(fields.indices, id: \.self) { i in
                                    let field = fields[i]
                                    let key = field.id
                                    VStack(alignment: .leading, spacing: 5) {
                                        HStack { Text(field.key).font(.caption.monospaced()); Spacer(); Text(field.unit).font(.caption).foregroundStyle(.secondary) }
                                        TextField(field.key, text: Binding(get: { state.values[key] ?? field.value }, set: { state.edit(key, $0) }))
                                            .textFieldStyle(.roundedBorder).accessibilityIdentifier(key)
                                        Text("Original \(field.original)").font(.system(size: 10)).foregroundStyle(.secondary)
                                    }
                                }
                            }
                        }
                        HStack {
                            Button("Undo") { state.undo() }.disabled(!state.canUndo)
                            Button("Redo") { state.redo() }.disabled(!state.canRedo)
                        }.padding(.top, 6)
                        Divider()
                        Text("Saving builds a separate copy; the game files are never changed. Test runs use their own save data.")
                            .font(.caption).foregroundStyle(.secondary)
                    } else if let detail = state.detail {
                        Text(detail.name).font(.headline)
                        LabeledContent("Type", value: detail.type)
                        LabeledContent("Size", value: "\(detail.size.formatted()) B")
                        if let meta = detail.metadata {
                            ForEach(meta.keys.sorted(), id: \.self) { key in LabeledContent(key, value: String(meta[key]!)) }
                        }
                        if let parts = detail.parts {
                            Divider()
                            Text("Parts").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                            ForEach(parts.indices, id: \.self) { index in Label(parts[index].name, systemImage: "cube").font(.caption) }
                        }
                    } else { Text("No asset selected").foregroundStyle(.secondary).font(.caption) }
                }.padding(14)
            }.disabled(state.busy || state.gameRunning)
        }.background(Color.white.opacity(0.025))
    }
}

struct ModsView: View {
    @ObservedObject var state: StudioState
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Button("Build Package") { Task { await state.buildPackage() } }
                    .disabled(state.busy || state.gameRunning || state.dirty || !state.hasModContent)
                Button("Install") { Task { await state.installPackage() } }
                    .disabled(state.busy || state.gameRunning || state.builtPackage == nil)
                if let built = state.builtPackage {
                    Text("\(built.id) \(built.version) · \(built.native) changed files · \(built.host) high-resolution textures")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
            }
            Divider()
            let packages = state.profile?.packages ?? []
            if packages.isEmpty {
                Text("No mods installed").foregroundStyle(.secondary)
            }
            ForEach(packages) { item in
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("\(item.id) \(item.version)").font(.system(.body, design: .monospaced))
                        Text(item.error ?? "\(item.native ?? 0) changed files · \(item.host ?? 0) high-resolution textures")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button(item.enabled ? "Disable" : "Enable") {
                        Task { await state.profileOperation(item.enabled ? "profile_disable" : "profile_enable", id: item.id) }
                    }
                    Button("Uninstall") { Task { await state.profileOperation("profile_uninstall", id: item.id) } }
                }.disabled(state.busy || state.gameRunning)
            }
            ForEach(state.profile?.conflicts.filter { !$0.accepted } ?? [], id: \.key) { conflict in
                Label("\(conflict.key) is changed by \(conflict.packages.joined(separator: ", "))", systemImage: "exclamationmark.triangle.fill")
                    .font(.callout).foregroundStyle(.orange)
            }
            Spacer()
        }.padding(16).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }
}

struct RetailMeshView: NSViewRepresentable {
    let parts: [MeshPart]
    let identity: String
    var wireframe = false
    final class Coordinator { var identity = "" }
    func makeCoordinator() -> Coordinator { Coordinator() }
    func makeNSView(context: Context) -> SCNView {
        let view = SCNView()
        view.allowsCameraControl = true
        view.autoenablesDefaultLighting = true
        view.backgroundColor = NSColor.windowBackgroundColor
        view.antialiasingMode = .multisampling4X
        return view
    }
    func updateNSView(_ view: SCNView, context: Context) {
        let version = identity + String(wireframe)
        guard context.coordinator.identity != version else { return }
        context.coordinator.identity = version
        let scene = SCNScene()
        let root = SCNNode()
        for part in parts {
            let positions = part.positions.map { SCNVector3(Float($0[0]), Float($0[1]), Float($0[2])) }
            let normals = part.normals.map { SCNVector3(Float($0[0]), Float($0[1]), Float($0[2])) }
            let uv = part.uvs.map { CGPoint(x: $0[0], y: $0[1]) }
            let indices = part.triangles.flatMap { $0 }
            let data = indices.withUnsafeBufferPointer { Data(buffer: $0) }
            let element = SCNGeometryElement(data: data, primitiveType: .triangles,
                primitiveCount: indices.count / 3, bytesPerIndex: MemoryLayout<Int32>.size)
            let geometry = SCNGeometry(sources: [SCNGeometrySource(vertices: positions),
                SCNGeometrySource(normals: normals), SCNGeometrySource(textureCoordinates: uv)], elements: [element])
            let material = SCNMaterial()
            material.diffuse.contents = part.texture.flatMap { NSImage(contentsOfFile: $0) } ?? NSColor.lightGray
            material.isDoubleSided = true
            material.diffuse.wrapS = .repeat
            material.diffuse.wrapT = .repeat
            material.fillMode = wireframe ? .lines : .fill
            geometry.materials = [material]
            let node = SCNNode(geometry: geometry); node.name = part.name
            root.addChildNode(node)
        }
        scene.rootNode.addChildNode(root)
        let bounds = root.boundingBox
        let center = SCNVector3((bounds.min.x + bounds.max.x) / 2, (bounds.min.y + bounds.max.y) / 2,
                               (bounds.min.z + bounds.max.z) / 2)
        let extent = max(bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y, bounds.max.z - bounds.min.z, 1)
        let camera = SCNNode(); camera.camera = SCNCamera()
        camera.camera?.zNear = 0.01; camera.camera?.zFar = Double(extent * 100)
        camera.position = SCNVector3(center.x + extent * 1.1, center.y + extent * 0.65, center.z + extent * 1.4)
        camera.look(at: center)
        scene.rootNode.addChildNode(camera)
        view.scene = scene; view.pointOfView = camera
        view.defaultCameraController.target = center
    }
}
