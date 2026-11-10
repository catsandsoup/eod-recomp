import SwiftUI

public struct StudioNavBar: View {
    @ObservedObject var state: AppState

    public init(state: AppState) {
        self.state = state
    }

    public var body: some View {
        HStack(spacing: 4) {
            ForEach(StudioTab.allCases) { tab in
                Button(action: {
                    state.activeStudio = tab
                }) {
                    HStack(spacing: 6) {
                        Image(systemName: tab.icon)
                            .font(.system(size: 13, weight: .semibold))
                        Text(tab.rawValue)
                            .font(.system(size: 12, weight: .medium))
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                    .background(
                        RoundedRectangle(cornerRadius: 6)
                            .fill(state.activeStudio == tab ? Color.blue.opacity(0.85) : Color.white.opacity(0.06))
                    )
                    .foregroundColor(state.activeStudio == tab ? .white : .secondary)
                }
                .buttonStyle(.plain)
            }
            Spacer()
            
            // Platform badge
            HStack(spacing: 6) {
                Text("Windows")
                    .font(.system(size: 10, weight: .medium))
                    .foregroundColor(.secondary)
                Circle().fill(Color.secondary.opacity(0.4)).frame(width: 3, height: 3)
                HStack(spacing: 3) {
                    Image(systemName: "applelogo")
                        .font(.system(size: 10))
                    Text("macOS")
                        .font(.system(size: 10, weight: .bold))
                }
                .foregroundColor(.white)
                .padding(.horizontal, 6)
                .padding(.vertical, 3)
                .background(Capsule().fill(Color.blue.opacity(0.3)))
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(Color(red: 0.12, green: 0.13, blue: 0.15))
    }
}
