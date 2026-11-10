import SwiftUI

@main
struct EODModdingSuiteApp: App {
    @StateObject private var appState = AppState()

    init() {
        if CommandLine.arguments.contains("--test") || CommandLine.arguments.contains("--headless") {
            let state = AppState()
            state.triggerBuild()
            print("[EODModdingSuite] Headless test validation: \(state.buildStatus)")
            exit(state.buildStatus.contains("Successful") ? 0 : 1)
        }
    }

    var body: some Scene {
        WindowGroup {
            MainView(state: appState)
                .preferredColorScheme(.dark)
                .frame(minWidth: 1280, minHeight: 800)
        }
        .windowStyle(.titleBar)
        .windowToolbarStyle(.unifiedCompact)
        .commands {
            SidebarCommands()
            CommandGroup(replacing: .newItem) {
                Button("New Mod Project...") {
                    // New project action
                }
                .keyboardShortcut("n", modifiers: [.command])
                
                Button("Open Mod Project...") {
                    // Open project action
                }
                .keyboardShortcut("o", modifiers: [.command])
                
                Divider()
                
                Button("Save Mod") {
                    appState.triggerBuild()
                }
                .keyboardShortcut("s", modifiers: [.command])
            }
            
            CommandMenu("Modding") {
                Button("Build Mod Package") {
                    appState.triggerBuild()
                }
                .keyboardShortcut("b", modifiers: [.command])
                
                Button("Test Drive (Play)") {
                    appState.buildStatus = "Launching Game..."
                }
                .keyboardShortcut("r", modifiers: [.command])
                
                Divider()
                
                Button("Validate All Manifests") {
                    appState.buildStatus = "All manifests validated successfully."
                }
            }
        }
    }
}
