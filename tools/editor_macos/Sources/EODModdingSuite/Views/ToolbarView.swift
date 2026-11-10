import SwiftUI

public struct ToolbarView: View {
    @ObservedObject var state: AppState

    public init(state: AppState) {
        self.state = state
    }

    public var body: some View {
        HStack(spacing: 8) {
            // Gizmo selector
            HStack(spacing: 2) {
                gizmoButton("Select", icon: "arrow.up.left")
                gizmoButton("Move", icon: "arrow.up.and.down.and.arrow.left.and.right")
                gizmoButton("Rotate", icon: "arrow.triangle.2.circlepath")
                gizmoButton("Scale", icon: "arrow.up.left.and.down.right.and.arrow.up.right.and.down.left")
            }
            .padding(2)
            .background(RoundedRectangle(cornerRadius: 6).fill(Color.white.opacity(0.05)))
            
            Divider().frame(height: 18).opacity(0.3)
            
            // Coordinate space
            Button(action: { state.isCoordinateGlobal.toggle() }) {
                HStack(spacing: 4) {
                    Image(systemName: state.isCoordinateGlobal ? "globe" : "cube")
                        .font(.system(size: 11))
                    Text(state.isCoordinateGlobal ? "Global" : "Local")
                        .font(.system(size: 11, weight: .medium))
                }
                .padding(.horizontal, 8)
                .padding(.vertical, 4)
                .background(RoundedRectangle(cornerRadius: 4).fill(Color.white.opacity(0.06)))
            }
            .buttonStyle(.plain)

            Divider().frame(height: 18).opacity(0.3)

            // Play & Test Actions
            Button(action: {
                state.launchGame(headless: false)
            }) {
                HStack(spacing: 5) {
                    Image(systemName: "play.fill")
                        .foregroundColor(.green)
                    Text("Play")
                        .fontWeight(.semibold)
                }
                .font(.system(size: 11))
                .padding(.horizontal, 10)
                .padding(.vertical, 4)
                .background(RoundedRectangle(cornerRadius: 5).fill(Color.green.opacity(0.2)))
            }
            .buttonStyle(.plain)

            Button(action: {
                state.buildStatus = "Simulating Physics..."
            }) {
                HStack(spacing: 5) {
                    Image(systemName: "gauge.with.needle.fill")
                        .foregroundColor(.cyan)
                    Text("Simulate")
                        .fontWeight(.semibold)
                }
                .font(.system(size: 11))
                .padding(.horizontal, 10)
                .padding(.vertical, 4)
                .background(RoundedRectangle(cornerRadius: 5).fill(Color.cyan.opacity(0.2)))
            }
            .buttonStyle(.plain)

            Button(action: {
                state.triggerBuild()
            }) {
                HStack(spacing: 5) {
                    Image(systemName: "shippingbox.fill")
                        .foregroundColor(.blue)
                    Text("Build Mod")
                        .fontWeight(.semibold)
                }
                .font(.system(size: 11))
                .padding(.horizontal, 10)
                .padding(.vertical, 4)
                .background(RoundedRectangle(cornerRadius: 5).fill(Color.blue.opacity(0.25)))
            }
            .buttonStyle(.plain)

            Spacer()

            // Camera speed indicator
            HStack(spacing: 4) {
                Image(systemName: "video.fill")
                    .font(.system(size: 10))
                    .foregroundColor(.secondary)
                Text(String(format: "Camera: %.1fx", state.cameraSpeed))
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundColor(.secondary)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(RoundedRectangle(cornerRadius: 4).fill(Color.black.opacity(0.25)))
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 5)
        .background(Color(red: 0.14, green: 0.15, blue: 0.18))
    }

    private func gizmoButton(_ name: String, icon: String) -> some View {
        Button(action: { state.selectedGizmo = name }) {
            Image(systemName: icon)
                .font(.system(size: 12))
                .frame(width: 24, height: 22)
                .background(state.selectedGizmo == name ? Color.blue : Color.clear)
                .foregroundColor(state.selectedGizmo == name ? .white : .secondary)
                .cornerRadius(4)
        }
        .buttonStyle(.plain)
    }
}
