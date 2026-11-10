import AppKit
import SceneKit
import SwiftUI

struct MainView: View {
    @ObservedObject var state: StudioState
    @State private var wireframe = false
    var body: some View {
        VStack(spacing: 0) {
            HStack {
                VStack(alignment: .leading, spacing: 3) {
                    Text("EVE OF DESTRUCTION").font(.system(size: 10, weight: .bold, design: .rounded)).tracking(2).foregroundStyle(.secondary)
                    Text("Creation Kit").font(.system(size: 19, weight: .semibold))
                }
                Divider().frame(height: 28).padding(.horizontal, 8)
                VStack(alignment: .leading, spacing: 4) {
                    Text(URL(fileURLWithPath: state.project).deletingLastPathComponent().lastPathComponent).font(.headline)
                    Text(state.editingAsset.isEmpty ? "Retail asset project" : "\(state.editingAsset)\(state.dirty ? " · Unsaved" : "")")
                        .font(.caption).foregroundStyle(state.dirty ? .orange : .secondary)
                }
                Spacer()
                Button("Open Project…") { state.openProject() }
                Button("Game Folder…") { state.chooseDisc() }
                Button("Save & Build") { state.save() }.disabled(!state.canSave)
                if state.gameRunning {
                    Button("Stop Test") { state.stopGame() }
                } else {
                    Button("Test \(state.savedTestArchive.map { URL(fileURLWithPath: $0).deletingPathExtension().lastPathComponent } ?? "Saved Mod")") { state.testGame() }.disabled(!state.canTest)
                }
            }
            .padding(12).disabled(state.busy)
            Divider()
            HStack(spacing: 6) {
                Label("Asset Workspace", systemImage: "square.stack.3d.up.fill")
                    .font(.system(size: 12, weight: .medium)).padding(.horizontal, 12).padding(.vertical, 7)
                    .background(Color.blue.opacity(0.22), in: RoundedRectangle(cornerRadius: 5))
                Spacer()
                Text(state.archive.isEmpty ? "Select a source archive" : state.archive).font(.caption.monospaced()).foregroundStyle(.secondary)
                if state.detail?.kind == "model" { Toggle("Wireframe", isOn: $wireframe).toggleStyle(.checkbox).font(.caption) }
            }.padding(.horizontal, 12).padding(.vertical, 7).background(Color.black.opacity(0.15))
            Divider()
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
                            Button("Export Native…") { state.exportNativeAsset() }.font(.caption).disabled(state.busy)
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
                            if state.error == nil { Text("Choose a vehicle, texture or model from the game sources.").foregroundStyle(.secondary) }
                        }.padding(24)
                        Spacer()
                    }
                }.frame(minWidth: 350, maxWidth: .infinity, maxHeight: .infinity)
                inspector.frame(minWidth: 250, idealWidth: 280, maxWidth: 340)
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
                if state.busy { ProgressView().controlSize(.small) }
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
                Text("\(parts.count) recovered mesh parts · \(parts.reduce(0) { $0 + $1.positions.count }.formatted()) vertices · \(parts.reduce(0) { $0 + $1.triangles.count }.formatted()) triangles")
                    .font(.caption).foregroundStyle(.secondary).padding(12)
                let issues = Array(Set(parts.compactMap(\.textureError))).sorted()
                if !issues.isEmpty {
                    Text(issues.joined(separator: "\n") + "\nUnavailable materials are shown in neutral gray.")
                        .font(.caption).foregroundStyle(.secondary).padding([.horizontal, .bottom], 12)
                }
            }
        } else if detail.kind == "texture", let path = detail.image, let image = NSImage(contentsOfFile: path) {
            ScrollView {
                Image(nsImage: image).resizable().interpolation(.none).scaledToFit().padding(20)
                if let meta = detail.metadata { Text("\(meta["width"] ?? 0) × \(meta["height"] ?? 0) · \(meta["levels"] ?? 0) mip levels").foregroundStyle(.secondary) }
            }
        } else if let sections = detail.sections {
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    ForEach(sections.indices, id: \.self) { index in
                        let section = sections[index]
                        DisclosureGroup("\(section.name) · \(section.fields.count) native fields") {
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
            ScrollView([.vertical, .horizontal]) {
                VStack(alignment: .leading, spacing: 12) {
                    Text(detail.message ?? "Native asset").foregroundStyle(.secondary)
                    Text(detail.hex ?? "").font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                }.padding(20)
            }
        }
    }

    private var inspector: some View {
        VStack(alignment: .leading, spacing: 0) {
            Label("Inspector", systemImage: "slider.horizontal.3").font(.caption.weight(.semibold))
                .foregroundStyle(.secondary).padding(12)
            Divider()
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    if let physics = state.physicsDetail, state.editingAsset.lowercased() == "physics.val" {
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
                                        Text("Retail \(field.original)").font(.system(size: 10)).foregroundStyle(.secondary)
                                    }
                                }
                            }
                        }
                        HStack {
                            Button("Undo") { state.undo() }.disabled(!state.canUndo)
                            Button("Redo") { state.redo() }.disabled(!state.canRedo)
                        }.padding(.top, 6)
                        Divider()
                        Text("Save & Build writes a separate native overlay. Test sessions use a separate memory card.")
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
                            Text("Recovered mesh hierarchy").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                            ForEach(parts.indices, id: \.self) { index in Label(parts[index].name, systemImage: "cube").font(.caption) }
                        }
                    } else { Text("No asset selected").foregroundStyle(.secondary).font(.caption) }
                }.padding(14)
            }.disabled(state.busy || state.gameRunning)
        }.background(Color.white.opacity(0.025))
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
