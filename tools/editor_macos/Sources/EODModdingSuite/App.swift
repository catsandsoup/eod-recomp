import SwiftUI

@MainActor
final class StudioAppDelegate: NSObject, NSApplicationDelegate {
    weak var state: StudioState?
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let state, state.prepareToQuit() else { return .terminateCancel }
        return .terminateNow
    }
}

@main
struct EODModdingSuiteApp: App {
    @StateObject private var state = StudioState()
    @NSApplicationDelegateAdaptor(StudioAppDelegate.self) private var delegate
    var body: some Scene {
        WindowGroup {
            MainView(state: state)
                .frame(minWidth: 1040, minHeight: 700)
                .preferredColorScheme(.dark)
                .onAppear { delegate.state = state }
                .task { await state.refresh() }
        }
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("New Mod Project…") { state.newProject() }.keyboardShortcut("n")
                Button("Open Mod Project…") { state.openProject() }.keyboardShortcut("o")
                Button("Save Project & Build Overlay") { state.save() }
                    .keyboardShortcut("s").disabled(!state.canSave)
            }
            CommandGroup(replacing: .undoRedo) {
                Button("Undo") { state.undo() }.keyboardShortcut("z").disabled(!state.canUndo)
                Button("Redo") { state.redo() }.keyboardShortcut("z", modifiers: [.command, .shift]).disabled(!state.canRedo)
            }
            CommandMenu("Game") {
                Button("Test Saved Mod in Game") { state.testGame() }.keyboardShortcut("r").disabled(!state.canTest)
                Button("Stop Test Session") { state.stopGame() }.disabled(!state.gameRunning)
                Divider()
                Button("Choose Game Installation…") { state.chooseDisc() }
                Button("Choose Game Executable…") { state.chooseExecutable() }
            }
        }
    }
}
