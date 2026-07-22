// HonestEQ — SwiftUI app entry point.
// Design: docs/ui-design.md
//
// Native macOS: uses standard SwiftUI controls only (Slider, Toggle, Menu,
// TextField). No custom-drawn knobs. Menu-bar hybrid handling comes in a
// follow-up (currently just a normal Dock app with a titled window).

import SwiftUI

@main
struct HonestEQApp: App {
    @StateObject private var state = AppState()

    var body: some Scene {
        WindowGroup("HonestEQ") {
            MainWindow()
                .environmentObject(state)
                .frame(minWidth: 900, minHeight: 600)
        }
        .windowStyle(.titleBar)
        .windowResizability(.contentSize)
        .commands {
            // Standard macOS menu commands.
            CommandGroup(replacing: .newItem) {
                Button("New Profile") { state.createNewProfile() }
                    .keyboardShortcut("n", modifiers: .command)
            }
            CommandGroup(after: .toolbar) {
                Button(state.isEqActive ? "Bypass" : "Activate EQ") {
                    state.isEqActive.toggle()
                }
                .keyboardShortcut("1", modifiers: .command)
            }
        }
    }
}
