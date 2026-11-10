import SwiftUI

public struct SceneHierarchyView: View {
    @ObservedObject var state: AppState
    @State private var searchScene: String = ""
    @State private var selectedNodeName: String = "Terrain"

    public init(state: AppState) {
        self.state = state
    }

    public var body: some View {
        VStack(spacing: 0) {
            // Header
            HStack {
                Image(systemName: "list.bullet.indent")
                    .foregroundColor(.blue)
                    .font(.system(size: 11))
                Text("Scene Hierarchy")
                    .font(.system(size: 12, weight: .bold))
                Spacer()
                Button(action: {}) {
                    Image(systemName: "plus")
                        .font(.system(size: 11))
                }
                .buttonStyle(.plain)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .background(Color(red: 0.14, green: 0.15, blue: 0.18))

            Divider().opacity(0.3)

            // Search Bar
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass")
                    .foregroundColor(.secondary)
                    .font(.system(size: 11))
                TextField("Search objects...", text: $searchScene)
                    .textFieldStyle(.plain)
                    .font(.system(size: 11))
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 5)
            .background(Color.black.opacity(0.25))
            .cornerRadius(5)
            .padding(6)

            Divider().opacity(0.3)

            // Tree View using native SwiftUI OutlineGroup
            List {
                OutlineGroup(state.sceneRoot, children: \.children) { node in
                    HStack(spacing: 6) {
                        Image(systemName: node.icon)
                            .foregroundColor(node.name.contains("Map") ? .yellow : (node.children != nil ? .blue : .secondary))
                            .font(.system(size: 11))
                        Text(node.name)
                            .font(.system(size: 11, weight: node.children != nil ? .semibold : .regular))
                        Spacer()
                    }
                    .padding(.vertical, 2)
                    .contentShape(Rectangle())
                    .onTapGesture {
                        selectedNodeName = node.name
                    }
                    .listRowBackground(
                        selectedNodeName == node.name ? Color.blue.opacity(0.25) : Color.clear
                    )
                }
            }
            .listStyle(.sidebar)
        }
        .background(Color(red: 0.13, green: 0.14, blue: 0.16))
    }
}
