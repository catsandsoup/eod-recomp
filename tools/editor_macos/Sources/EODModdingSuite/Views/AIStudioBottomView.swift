import SwiftUI

public struct AIStudioBottomView: View {
    @ObservedObject var state: AppState
    @State private var activeTab: String = "Waypoints"
    @State private var speedLimit: Double = 150.0
    @State private var behavior: String = "Normal"
    @State private var allowOvertake: Bool = true
    @State private var avoidCollisions: Bool = true
    @State private var useRacingLine: Bool = true

    public init(state: AppState) {
        self.state = state
    }

    public var body: some View {
        HStack(spacing: 0) {
            // Left: 2D Track Spline & Waypoints Canvas
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("AI Studio — Racing Lines & Pathfinding")
                        .font(.system(size: 11, weight: .bold))
                    Spacer()
                    // Legend
                    HStack(spacing: 12) {
                        legendItem(color: .blue, text: "Racing Line")
                        legendItem(color: .yellow, text: "AI Waypoint")
                        legendItem(color: .green, text: "Branch")
                        legendItem(color: .cyan, text: "Speed Zone")
                        legendItem(color: .red, text: "Avoid Zone")
                    }
                }
                .padding(.horizontal, 10)
                .padding(.top, 6)

                // Interactive 2D Track Canvas
                GeometryReader { geo in
                    ZStack {
                        // Track background (dirt/oval look)
                        RoundedRectangle(cornerRadius: 8)
                            .fill(Color(red: 0.10, green: 0.11, blue: 0.13))

                        // Infield oval
                        Ellipse()
                            .fill(Color(red: 0.15, green: 0.16, blue: 0.19))
                            .frame(width: geo.size.width * 0.6, height: geo.size.height * 0.5)

                        // Outer track boundary
                        Ellipse()
                            .stroke(Color.white.opacity(0.15), lineWidth: 32)
                            .frame(width: geo.size.width * 0.78, height: geo.size.height * 0.78)

                        // Racing line curve
                        Path { path in
                            let center = CGPoint(x: geo.size.width / 2, y: geo.size.height / 2)
                            let rx = geo.size.width * 0.38
                            let ry = geo.size.height * 0.38
                            path.addEllipse(in: CGRect(x: center.x - rx, y: center.y - ry, width: rx * 2, height: ry * 2))
                        }
                        .stroke(
                            LinearGradient(colors: [.blue, .cyan, .green, .yellow, .blue], startPoint: .topLeading, endPoint: .bottomTrailing),
                            style: StrokeStyle(lineWidth: 3, lineCap: .round, dash: [6, 4])
                        )

                        // Render AI Waypoints
                        ForEach(state.waypoints) { wp in
                            let normalizedPos = CGPoint(
                                x: (wp.position.x / 500.0) * geo.size.width,
                                y: (wp.position.y / 500.0) * geo.size.height
                            )
                            Circle()
                                .fill(wp.isBranch ? Color.green : (state.selectedWaypointId == wp.id ? Color.white : Color.yellow))
                                .frame(width: state.selectedWaypointId == wp.id ? 14 : 10, height: state.selectedWaypointId == wp.id ? 14 : 10)
                                .overlay(
                                    Circle().stroke(Color.black, lineWidth: 2)
                                )
                                .position(normalizedPos)
                                .onTapGesture {
                                    state.selectedWaypointId = wp.id
                                    speedLimit = wp.speedLimitKmh
                                }
                        }
                    }
                }
                .padding(6)
            }
            .frame(maxWidth: .infinity)

            Divider().opacity(0.3)

            // Right: Waypoint Properties Inspector
            VStack(alignment: .leading, spacing: 8) {
                Text("Waypoint Inspector")
                    .font(.system(size: 11, weight: .bold))
                    .padding(.top, 6)

                if let selId = state.selectedWaypointId {
                    Text("Waypoint #\(selId)")
                        .font(.system(size: 10, weight: .semibold, design: .monospaced))
                        .foregroundColor(.blue)

                    VStack(alignment: .leading, spacing: 2) {
                        HStack {
                            Text("Speed Limit")
                                .font(.system(size: 10))
                                .foregroundColor(.secondary)
                            Spacer()
                            Text(String(format: "%.0f km/h", speedLimit))
                                .font(.system(size: 10, weight: .bold))
                        }
                        Slider(value: $speedLimit, in: 40...280, step: 5)
                    }

                    VStack(alignment: .leading, spacing: 2) {
                        Text("Behaviour")
                            .font(.system(size: 10))
                            .foregroundColor(.secondary)
                        Picker("", selection: $behavior) {
                            Text("Normal").tag("Normal")
                            Text("Aggressive").tag("Aggressive")
                            Text("Defensive").tag("Defensive")
                            Text("Drafting").tag("Drafting")
                        }
                        .labelsHidden()
                        .pickerStyle(.menu)
                    }

                    VStack(alignment: .leading, spacing: 4) {
                        Toggle("Allow Overtake", isOn: $allowOvertake)
                        Toggle("Avoid Collisions", isOn: $avoidCollisions)
                        Toggle("Use Racing Line", isOn: $useRacingLine)
                    }
                    .font(.system(size: 10))
                    .toggleStyle(.checkbox)

                    Spacer()

                    HStack {
                        Button(action: {
                            let nextId = state.waypoints.count
                            state.waypoints.append(AIWaypoint(id: nextId, position: CGPoint(x: 250, y: 250)))
                            state.selectedWaypointId = nextId
                        }) {
                            HStack(spacing: 4) {
                                Image(systemName: "plus")
                                Text("Add Waypoint")
                            }
                            .font(.system(size: 10))
                            .padding(.horizontal, 8)
                            .padding(.vertical, 4)
                            .background(Color.blue.opacity(0.3))
                            .cornerRadius(4)
                        }
                        .buttonStyle(.plain)

                        Button(action: {
                            if let idx = state.waypoints.firstIndex(where: { $0.id == state.selectedWaypointId }) {
                                state.waypoints.remove(at: idx)
                                state.selectedWaypointId = state.waypoints.first?.id
                            }
                        }) {
                            Text("Delete")
                                .font(.system(size: 10))
                                .foregroundColor(.red)
                                .padding(.horizontal, 8)
                                .padding(.vertical, 4)
                                .background(Color.red.opacity(0.15))
                                .cornerRadius(4)
                        }
                        .buttonStyle(.plain)
                    }
                } else {
                    Text("Select a waypoint to edit properties.")
                        .font(.system(size: 10))
                        .foregroundColor(.secondary)
                    Spacer()
                }
            }
            .frame(width: 190)
            .padding(.horizontal, 10)
            .padding(.bottom, 8)
            .background(Color(red: 0.13, green: 0.14, blue: 0.16))
        }
        .background(Color(red: 0.11, green: 0.12, blue: 0.14))
    }

    private func legendItem(color: Color, text: String) -> some View {
        HStack(spacing: 4) {
            Circle().fill(color).frame(width: 6, height: 6)
            Text(text)
                .font(.system(size: 9))
                .foregroundColor(.secondary)
        }
    }
}
