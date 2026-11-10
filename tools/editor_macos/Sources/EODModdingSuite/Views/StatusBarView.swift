import SwiftUI

public struct StatusBarView: View {
    @ObservedObject var state: AppState

    public init(state: AppState) {
        self.state = state
    }

    public var body: some View {
        HStack(spacing: 12) {
            // Project directory indicator
            HStack(spacing: 6) {
                Image(systemName: "folder.fill")
                    .foregroundColor(.blue)
                    .font(.system(size: 10))
                Text("Project: \(state.projectName)")
                    .font(.system(size: 11, weight: .semibold))
                Text("•")
                    .foregroundColor(.secondary.opacity(0.5))
                Text(state.projectPath)
                    .font(.system(size: 10, design: .monospaced))
                    .foregroundColor(.secondary)
            }

            Spacer()

            // Build Status
            HStack(spacing: 6) {
                Circle()
                    .fill(state.buildStatus.contains("Building") ? Color.yellow : Color.green)
                    .frame(width: 7, height: 7)
                Text(state.buildStatus)
                    .font(.system(size: 11, weight: .medium))
                    .foregroundColor(state.buildStatus.contains("Building") ? .yellow : .green)
            }

            Spacer()

            // Memory Monitor & Readiness
            HStack(spacing: 10) {
                HStack(spacing: 6) {
                    Text("Memory: 3.1 GB / 16 GB")
                        .font(.system(size: 10, design: .monospaced))
                        .foregroundColor(.secondary)

                    // Compact memory bar
                    GeometryReader { geo in
                        ZStack(alignment: .leading) {
                            Capsule().fill(Color.white.opacity(0.1))
                            Capsule().fill(Color.green)
                                .frame(width: geo.size.width * 0.20)
                        }
                    }
                    .frame(width: 45, height: 6)
                }

                HStack(spacing: 4) {
                    Circle().fill(Color.green).frame(width: 6, height: 6)
                    Text("Ready")
                        .font(.system(size: 10, weight: .medium))
                        .foregroundColor(.secondary)
                }
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 5)
        .background(Color(red: 0.10, green: 0.11, blue: 0.13))
        .overlay(
            Rectangle()
                .frame(height: 1)
                .foregroundColor(Color.white.opacity(0.06)),
            alignment: .top
        )
    }
}
