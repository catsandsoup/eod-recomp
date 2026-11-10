import SwiftUI

public struct WorldEditorBottomView: View {
    @ObservedObject var state: AppState
    @State private var terrainTool: String = "Raise"
    @State private var brushSize: Double = 24.0
    @State private var brushStrength: Double = 0.75
    @State private var currentHeight: Double = 42.8

    private let tools = [
        ("Raise", "arrow.up.circle.fill"),
        ("Lower", "arrow.down.circle.fill"),
        ("Smooth", "water.waves"),
        ("Flatten", "square.fill"),
        ("Ramp", "skew")
    ]

    public init(state: AppState) {
        self.state = state
    }

    public var body: some View {
        HStack(spacing: 0) {
            // Left: Terrain Tools & 3D Aerial Terrain Preview
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("World Editor — Terrain & Surface Sculpting")
                        .font(.system(size: 11, weight: .bold))
                    Spacer()
                    Text("X: 243.7  Y: 87.2  Z: 312.9  |  Height: \(String(format: "%.1fm", currentHeight))")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(.secondary)
                }
                .padding(.horizontal, 10)
                .padding(.top, 6)

                // Tool Palette + Heightmap curve
                HStack(spacing: 12) {
                    // Tool buttons
                    VStack(spacing: 4) {
                        ForEach(tools, id: \.0) { name, icon in
                            Button(action: { terrainTool = name }) {
                                HStack(spacing: 6) {
                                    Image(systemName: icon)
                                        .font(.system(size: 11))
                                    Text(name)
                                        .font(.system(size: 10))
                                    Spacer()
                                }
                                .padding(.horizontal, 8)
                                .padding(.vertical, 4)
                                .frame(width: 90)
                                .background(terrainTool == name ? Color.blue : Color.white.opacity(0.05))
                                .foregroundColor(terrainTool == name ? .white : .secondary)
                                .cornerRadius(4)
                            }
                            .buttonStyle(.plain)
                        }
                    }

                    // Height Curve Graph / Elevation Profile
                    ZStack {
                        RoundedRectangle(cornerRadius: 6)
                            .fill(Color(red: 0.10, green: 0.11, blue: 0.13))

                        VStack {
                            Path { path in
                                path.move(to: CGPoint(x: 20, y: 110))
                                path.addQuadCurve(to: CGPoint(x: 140, y: 30), control: CGPoint(x: 80, y: 105))
                                path.addQuadCurve(to: CGPoint(x: 280, y: 110), control: CGPoint(x: 200, y: 25))
                            }
                            .stroke(Color.cyan, lineWidth: 2)

                            Spacer()

                            HStack {
                                Text("Terrain Profile Curve")
                                    .font(.system(size: 9))
                                    .foregroundColor(.secondary)
                                Spacer()
                                Text("Elevation: 0m - 120m")
                                    .font(.system(size: 9))
                                    .foregroundColor(.secondary)
                            }
                            .padding(.horizontal, 8)
                            .padding(.bottom, 4)
                        }
                    }
                }
                .padding(6)
            }
            .frame(maxWidth: .infinity)

            Divider().opacity(0.3)

            // Right: Brush Inspector
            VStack(alignment: .leading, spacing: 8) {
                Text("Terrain Brush")
                    .font(.system(size: 11, weight: .bold))
                    .padding(.top, 6)

                VStack(alignment: .leading, spacing: 2) {
                    HStack {
                        Text("Brush Size")
                            .font(.system(size: 10))
                            .foregroundColor(.secondary)
                        Spacer()
                        Text(String(format: "%.1f m", brushSize))
                            .font(.system(size: 10, weight: .bold))
                    }
                    Slider(value: $brushSize, in: 2...100)
                }

                VStack(alignment: .leading, spacing: 2) {
                    HStack {
                        Text("Brush Strength")
                            .font(.system(size: 10))
                            .foregroundColor(.secondary)
                        Spacer()
                        Text(String(format: "%.2f", brushStrength))
                            .font(.system(size: 10, weight: .bold))
                    }
                    Slider(value: $brushStrength, in: 0.05...1.0)
                }

                VStack(alignment: .leading, spacing: 2) {
                    Text("Falloff Mode")
                        .font(.system(size: 10))
                        .foregroundColor(.secondary)
                    Picker("", selection: .constant("Smooth Cosine")) {
                        Text("Smooth Cosine").tag("Smooth Cosine")
                        Text("Linear").tag("Linear")
                        Text("Spherical").tag("Spherical")
                        Text("Flat").tag("Flat")
                    }
                    .labelsHidden()
                    .pickerStyle(.menu)
                }

                Spacer()

                Button("Reset Terrain Flatten") {
                    currentHeight = 0.0
                }
                .font(.system(size: 10))
                .foregroundColor(.secondary)
                .buttonStyle(.plain)
            }
            .frame(width: 190)
            .padding(.horizontal, 10)
            .padding(.bottom, 8)
            .background(Color(red: 0.13, green: 0.14, blue: 0.16))
        }
        .background(Color(red: 0.11, green: 0.12, blue: 0.14))
    }
}
