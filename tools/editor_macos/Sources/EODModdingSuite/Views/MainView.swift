import SwiftUI

public struct MainView: View {
    @ObservedObject var state: AppState
    @State private var bottomDrawerHeight: CGFloat = 260

    public init(state: AppState) {
        self.state = state
    }

    public var body: some View {
        VStack(spacing: 0) {
            // Top Navigation Studio Switcher
            StudioNavBar(state: state)

            // Top Action Toolbar & Gizmos
            ToolbarView(state: state)

            // Main Editor 3-Column Workspace
            HSplitView {
                // Left Column: Asset Browser
                AssetBrowserView(state: state)
                    .frame(minWidth: 260, idealWidth: 300, maxWidth: 420)

                // Center Column: 3D Viewport on top, Studio Drawer on bottom
                VSplitView {
                    // 3D Hardware Accelerated Viewport
                    Viewport3DView(state: state)
                        .frame(minHeight: 300)

                    // Bottom Studio Drawer (Context-sensitive)
                    VStack(spacing: 0) {
                        // Drawer Tab Header
                        HStack(spacing: 4) {
                            drawerTabButton("Vehicle Studio", icon: "car.fill")
                            drawerTabButton("AI Studio", icon: "brain.head.profile")
                            drawerTabButton("World Editor", icon: "mountain.2.fill")
                            drawerTabButton("Event Editor", icon: "flag.checkered.2.crossed")
                            drawerTabButton("Game Preview", icon: "play.tv.fill")

                            Spacer()

                            Button(action: {}) {
                                Image(systemName: "chevron.down")
                                    .font(.system(size: 10))
                                    .foregroundColor(.secondary)
                            }
                            .buttonStyle(.plain)
                        }
                        .padding(.horizontal, 10)
                        .padding(.vertical, 4)
                        .background(Color(red: 0.14, green: 0.15, blue: 0.18))

                        Divider().opacity(0.3)

                        // Active Drawer Content
                        Group {
                            switch state.activeBottomTab {
                            case "AI Studio":
                                AIStudioBottomView(state: state)
                            case "World Editor":
                                WorldEditorBottomView(state: state)
                            case "Event Editor":
                                EventEditorBottomView(state: state)
                            case "Game Preview":
                                GamePreviewBottomView(state: state)
                            default:
                                VehicleStudioBottomView(state: state)
                            }
                        }
                    }
                    .frame(minHeight: 180, idealHeight: 260, maxHeight: 380)
                }

                // Right Column: Properties Inspector & Scene Hierarchy
                VSplitView {
                    PropertiesInspectorView(state: state)
                        .frame(minHeight: 250)

                    SceneHierarchyView(state: state)
                        .frame(minHeight: 180)
                }
                .frame(minWidth: 240, idealWidth: 280, maxWidth: 360)
            }

            // Bottom System Status Bar
            StatusBarView(state: state)
        }
        .background(Color(red: 0.10, green: 0.11, blue: 0.13))
        .onChange(of: state.activeStudio) { _, newStudio in
            switch newStudio {
            case .vehicleStudio:
                state.activeBottomTab = "Vehicle Studio"
            case .aiStudio:
                state.activeBottomTab = "AI Studio"
            case .worldStudio:
                state.activeBottomTab = "World Editor"
            case .eventStudio:
                state.activeBottomTab = "Event Editor"
            case .buildTest:
                state.activeBottomTab = "Game Preview"
            default:
                break
            }
        }
    }

    private func drawerTabButton(_ name: String, icon: String) -> some View {
        Button(action: { state.activeBottomTab = name }) {
            HStack(spacing: 5) {
                Image(systemName: icon)
                    .font(.system(size: 10))
                Text(name)
                    .font(.system(size: 10, weight: state.activeBottomTab == name ? .bold : .medium))
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .background(
                state.activeBottomTab == name ? Color.blue.opacity(0.8) : Color.white.opacity(0.04)
            )
            .foregroundColor(state.activeBottomTab == name ? .white : .secondary)
            .cornerRadius(4)
        }
        .buttonStyle(.plain)
    }
}
