import SwiftUI
import SceneKit

public struct Viewport3DView: View {
    @ObservedObject var state: AppState
    @State private var viewMode: String = "Lit"
    @State private var cameraPreset: String = "Perspective"
    @State private var showStats: Bool = true
    @State private var showGrid: Bool = true

    public init(state: AppState) {
        self.state = state
    }

    public var body: some View {
        ZStack(alignment: .topLeading) {
            // 3D Scene View
            SceneKitContainer(deformationAmount: state.vehicle.deformationAmount, showGrid: showGrid)
                .edgesIgnoringSafeArea(.all)

            // Top HUD Overlay Controls
            HStack(spacing: 8) {
                // Camera View Preset
                Menu {
                    Button("Perspective") { cameraPreset = "Perspective" }
                    Button("Top Down") { cameraPreset = "Top Down" }
                    Button("Front") { cameraPreset = "Front" }
                    Button("Side") { cameraPreset = "Side" }
                } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "camera.aperture")
                        Text(cameraPreset)
                        Image(systemName: "chevron.down").font(.system(size: 8))
                    }
                    .font(.system(size: 11, weight: .medium))
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
                    .background(Color.black.opacity(0.65))
                    .cornerRadius(5)
                }
                .menuStyle(.borderlessButton)

                // Shading Mode
                Menu {
                    Button("Lit") { viewMode = "Lit" }
                    Button("Wireframe") { viewMode = "Wireframe" }
                    Button("Unlit") { viewMode = "Unlit" }
                    Button("Normals") { viewMode = "Normals" }
                } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "circle.lefthalf.filled")
                        Text(viewMode)
                        Image(systemName: "chevron.down").font(.system(size: 8))
                    }
                    .font(.system(size: 11, weight: .medium))
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
                    .background(Color.black.opacity(0.65))
                    .cornerRadius(5)
                }
                .menuStyle(.borderlessButton)

                // Visibility Toggles
                Button(action: { showGrid.toggle() }) {
                    HStack(spacing: 4) {
                        Image(systemName: showGrid ? "grid" : "grid.circle")
                        Text("Grid")
                    }
                    .font(.system(size: 11))
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
                    .background(showGrid ? Color.blue.opacity(0.6) : Color.black.opacity(0.65))
                    .cornerRadius(5)
                }
                .buttonStyle(.plain)

                Spacer()

                // Top Right: Coordinate Gizmo Legend
                HStack(spacing: 8) {
                    HStack(spacing: 2) {
                        Circle().fill(Color.red).frame(width: 6, height: 6)
                        Text("X").font(.system(size: 10, weight: .bold)).foregroundColor(.red)
                    }
                    HStack(spacing: 2) {
                        Circle().fill(Color.green).frame(width: 6, height: 6)
                        Text("Y").font(.system(size: 10, weight: .bold)).foregroundColor(.green)
                    }
                    HStack(spacing: 2) {
                        Circle().fill(Color.blue).frame(width: 6, height: 6)
                        Text("Z").font(.system(size: 10, weight: .bold)).foregroundColor(.blue)
                    }
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 4)
                .background(Color.black.opacity(0.65))
                .cornerRadius(5)
            }
            .padding(10)

            // Performance & Engine Overlay Card (Top Left below menu)
            if showStats {
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 6) {
                        Circle().fill(Color.green).frame(width: 6, height: 6)
                        Text("FPS: \(state.fps)")
                            .foregroundColor(.white)
                    }
                    Text("Draw Calls: \(state.drawCalls)")
                        .foregroundColor(.secondary)
                    Text("Tris: \(state.trisCount)")
                        .foregroundColor(.secondary)
                    Text("Mem: \(state.memoryUsage)")
                        .foregroundColor(.secondary)
                }
                .font(.system(size: 10, weight: .semibold, design: .monospaced))
                .padding(8)
                .background(Color.black.opacity(0.75))
                .cornerRadius(6)
                .overlay(
                    RoundedRectangle(cornerRadius: 6)
                        .stroke(Color.white.opacity(0.1), lineWidth: 1)
                )
                .padding(.top, 45)
                .padding(.leading, 10)
            }

            // Bottom Viewport Controls
            VStack {
                Spacer()
                HStack {
                    HStack(spacing: 6) {
                        Image(systemName: "video.fill")
                            .font(.system(size: 10))
                            .foregroundColor(.secondary)
                        Text("Speed:")
                            .font(.system(size: 10))
                            .foregroundColor(.secondary)
                        Slider(value: $state.cameraSpeed, in: 0.2...3.0)
                            .frame(width: 80)
                            .controlSize(.mini)
                        Text(String(format: "%.1fx", state.cameraSpeed))
                            .font(.system(size: 10, design: .monospaced))
                            .foregroundColor(.white)
                    }
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
                    .background(Color.black.opacity(0.7))
                    .cornerRadius(5)

                    Spacer()

                    // Quick view reset
                    Button(action: {}) {
                        HStack(spacing: 4) {
                            Image(systemName: "arrow.counterclockwise")
                            Text("Reset View")
                        }
                        .font(.system(size: 10))
                        .padding(.horizontal, 8)
                        .padding(.vertical, 4)
                        .background(Color.black.opacity(0.7))
                        .cornerRadius(5)
                    }
                    .buttonStyle(.plain)
                }
                .padding(10)
            }
        }
    }
}

// SceneKit View Representation
private struct SceneKitContainer: NSViewRepresentable {
    var deformationAmount: Double
    var showGrid: Bool

    func makeNSView(context: Context) -> SCNView {
        let scnView = SCNView()
        scnView.scene = createScene()
        scnView.allowsCameraControl = true
        scnView.autoenablesDefaultLighting = false
        scnView.backgroundColor = NSColor(calibratedRed: 0.08, green: 0.09, blue: 0.11, alpha: 1.0)
        scnView.antialiasingMode = .multisampling4X
        return scnView
    }

    func updateNSView(_ nsView: SCNView, context: Context) {
        // Adjust vehicle deformation or mesh state when deformationAmount changes
        if let carNode = nsView.scene?.rootNode.childNode(withName: "car_chassis", recursively: true) {
            let scaleY = 1.0 - (deformationAmount * 0.25)
            let rotZ = CGFloat(deformationAmount * 0.15)
            carNode.scale = SCNVector3(1.0, scaleY, 1.0)
            carNode.eulerAngles.z = rotZ
        }
        
        if let gridNode = nsView.scene?.rootNode.childNode(withName: "ground_grid", recursively: true) {
            gridNode.isHidden = !showGrid
        }
    }

    private func createScene() -> SCNScene {
        let scene = SCNScene()

        // 1. Camera Node
        let cameraNode = SCNNode()
        cameraNode.camera = SCNCamera()
        cameraNode.position = SCNVector3(x: 4.5, y: 3.2, z: 5.5)
        cameraNode.look(at: SCNVector3(x: 0, y: 0.8, z: 0))
        scene.rootNode.addChildNode(cameraNode)

        // 2. Sun Light (Directional with shadows)
        let sunNode = SCNNode()
        let sun = SCNLight()
        sun.type = .directional
        sun.color = NSColor(calibratedWhite: 0.95, alpha: 1.0)
        sun.castsShadow = true
        sun.shadowRadius = 4.0
        sun.shadowColor = NSColor.black.withAlphaComponent(0.6)
        sunNode.light = sun
        sunNode.eulerAngles = SCNVector3(x: -CGFloat.pi / 3, y: CGFloat.pi / 4, z: 0)
        scene.rootNode.addChildNode(sunNode)

        // 3. Ambient & Fill Light
        let ambientNode = SCNNode()
        let ambient = SCNLight()
        ambient.type = .ambient
        ambient.color = NSColor(calibratedRed: 0.2, green: 0.22, blue: 0.26, alpha: 1.0)
        ambientNode.light = ambient
        scene.rootNode.addChildNode(ambientNode)

        // 4. Ground Grid Floor
        let groundGeo = SCNFloor()
        groundGeo.reflectivity = 0.08
        groundGeo.reflectionFalloffEnd = 8.0
        let floorMat = SCNMaterial()
        floorMat.diffuse.contents = NSColor(calibratedRed: 0.12, green: 0.13, blue: 0.15, alpha: 1.0)
        groundGeo.materials = [floorMat]
        let groundNode = SCNNode(geometry: groundGeo)
        groundNode.name = "ground_grid"
        scene.rootNode.addChildNode(groundNode)

        // Ground Guide Ring / Grid Lines
        let gridRings = SCNNode()
        for r in [2.0, 4.0, 6.0, 8.0] {
            let tube = SCNTorus(ringRadius: r, pipeRadius: 0.015)
            tube.firstMaterial?.diffuse.contents = NSColor.white.withAlphaComponent(0.12)
            let ringNode = SCNNode(geometry: tube)
            gridRings.addChildNode(ringNode)
        }
        gridRings.name = "grid_rings"
        groundNode.addChildNode(gridRings)

        // 5. Procedural 3D Car Root Node
        let carRoot = SCNNode()
        carRoot.name = "vehicle_root"
        carRoot.position = SCNVector3(0, 0.45, 0)

        // Car Body (Chassis)
        let bodyGeo = SCNBox(width: 1.9, height: 0.55, length: 4.2, chamferRadius: 0.08)
        let bodyMat = SCNMaterial()
        bodyMat.diffuse.contents = NSColor(calibratedRed: 0.78, green: 0.22, blue: 0.18, alpha: 1.0) // Muscle Car Red
        bodyMat.metalness.contents = 0.8
        bodyMat.roughness.contents = 0.35
        bodyGeo.materials = [bodyMat]
        let chassisNode = SCNNode(geometry: bodyGeo)
        chassisNode.name = "car_chassis"
        chassisNode.position = SCNVector3(0, 0.3, 0)
        carRoot.addChildNode(chassisNode)

        // Cabin Roof
        let cabinGeo = SCNBox(width: 1.6, height: 0.5, length: 2.1, chamferRadius: 0.05)
        let cabinMat = SCNMaterial()
        cabinMat.diffuse.contents = NSColor(calibratedRed: 0.15, green: 0.15, blue: 0.18, alpha: 0.9)
        cabinGeo.materials = [cabinMat]
        let cabinNode = SCNNode(geometry: cabinGeo)
        cabinNode.position = SCNVector3(0, 0.52, -0.2)
        chassisNode.addChildNode(cabinNode)

        // Hood Scoop
        let scoopGeo = SCNBox(width: 0.6, height: 0.12, length: 0.9, chamferRadius: 0.02)
        scoopGeo.firstMaterial?.diffuse.contents = NSColor.black
        let scoopNode = SCNNode(geometry: scoopGeo)
        scoopNode.position = SCNVector3(0, 0.32, 1.1)
        chassisNode.addChildNode(scoopNode)

        // Front Bumper (Detachable)
        let frontBumperGeo = SCNBox(width: 1.95, height: 0.2, length: 0.2, chamferRadius: 0.03)
        frontBumperGeo.firstMaterial?.diffuse.contents = NSColor(calibratedWhite: 0.85, alpha: 1.0)
        let frontBumper = SCNNode(geometry: frontBumperGeo)
        frontBumper.name = "front_bumper"
        frontBumper.position = SCNVector3(0, 0, 2.15)
        chassisNode.addChildNode(frontBumper)

        // Rear Bumper (Detachable)
        let rearBumperGeo = SCNBox(width: 1.95, height: 0.2, length: 0.2, chamferRadius: 0.03)
        rearBumperGeo.firstMaterial?.diffuse.contents = NSColor(calibratedWhite: 0.85, alpha: 1.0)
        let rearBumper = SCNNode(geometry: rearBumperGeo)
        rearBumper.name = "rear_bumper"
        rearBumper.position = SCNVector3(0, 0, -2.15)
        chassisNode.addChildNode(rearBumper)

        // 4 Wheels
        let wheelRadius: CGFloat = 0.36
        let wheelWidth: CGFloat = 0.26
        let wheelPositions: [(CGFloat, CGFloat, CGFloat)] = [
            (-0.95, -0.05, 1.3),  // Front Left
            (0.95, -0.05, 1.3),   // Front Right
            (-0.95, -0.05, -1.3), // Rear Left
            (0.95, -0.05, -1.3)   // Rear Right
        ]

        for (x, y, z) in wheelPositions {
            let tireGeo = SCNCylinder(radius: wheelRadius, height: wheelWidth)
            let tireMat = SCNMaterial()
            tireMat.diffuse.contents = NSColor(calibratedWhite: 0.15, alpha: 1.0)
            tireMat.roughness.contents = 0.9
            tireGeo.materials = [tireMat]
            let wheelNode = SCNNode(geometry: tireGeo)
            wheelNode.eulerAngles = SCNVector3(0, 0, CGFloat.pi / 2)
            wheelNode.position = SCNVector3(x, y, z)

            // Chrome rim center
            let rimGeo = SCNCylinder(radius: wheelRadius * 0.65, height: wheelWidth + 0.02)
            let rimMat = SCNMaterial()
            rimMat.diffuse.contents = NSColor(calibratedWhite: 0.85, alpha: 1.0)
            rimMat.metalness.contents = 0.95
            rimGeo.materials = [rimMat]
            let rimNode = SCNNode(geometry: rimGeo)
            wheelNode.addChildNode(rimNode)

            carRoot.addChildNode(wheelNode)
        }

        scene.rootNode.addChildNode(carRoot)
        return scene
    }
}
