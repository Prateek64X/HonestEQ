// MainWindow.swift — top-level layout.
// docs/ui-design.md §"Main window — expanded/collapsed"

import SwiftUI

struct MainWindow: View {
    @EnvironmentObject var state: AppState
    @State private var showSidebar = false   // v1 defaults collapsed; toggle via toolbar

    var body: some View {
        HSplitView {
            mainContent
                .frame(minWidth: 640)

            if showSidebar {
                DevicesSidebar()
                    .frame(minWidth: 260, idealWidth: 300, maxWidth: 380)
            }
        }
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button {
                    withAnimation(.easeInOut(duration: 0.2)) {
                        showSidebar.toggle()
                    }
                } label: {
                    Label(showSidebar ? "Hide Sidebar" : "Show Sidebar",
                          systemImage: "sidebar.right")
                }
            }
        }
    }

    private var mainContent: some View {
        VStack(spacing: 12) {
            TopControlsView()
            Divider()
            BandEditorView()
            Divider()
            ProfileSection()
        }
        // Zero top padding — Pre-Amp label sits right under the titlebar.
        // Side and bottom padding stay comfortable at 16 pt.
        .padding(EdgeInsets(top: 0, leading: 16, bottom: 16, trailing: 16))
    }
}
