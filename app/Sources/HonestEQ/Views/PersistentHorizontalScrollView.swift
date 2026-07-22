// PersistentHorizontalScrollView.swift
//
// SwiftUI's built-in ScrollView respects the macOS system-wide "Show scroll
// bars" setting — if the user has it on "Automatically based on mouse or
// trackpad" (the default with a trackpad), scrollbars only flash briefly
// during scroll gestures. That leaves users guessing whether there's more
// content off-screen.
//
// This wraps NSScrollView with:
//   scrollerStyle       = .legacy        → classic bar at the bottom
//   autohidesScrollers  = false          → never hides
//   hasVerticalScroller = false          → horizontal only
//
// Behaves the same as Finder's list-view horizontal scrollbar — always
// visible when the content is wider than the view, disappears cleanly when
// the window is widened enough that everything fits.

import SwiftUI
import AppKit

struct PersistentHorizontalScrollView<Content: View>: NSViewRepresentable {
    let content: Content

    init(@ViewBuilder content: () -> Content) {
        self.content = content()
    }

    func makeNSView(context: Context) -> NSScrollView {
        let scrollView = NSScrollView()
        scrollView.hasHorizontalScroller  = true
        scrollView.hasVerticalScroller    = false
        scrollView.autohidesScrollers     = false
        scrollView.scrollerStyle          = .legacy
        scrollView.horizontalScrollElasticity = .automatic
        scrollView.borderType             = .noBorder
        scrollView.drawsBackground        = false

        let hosting = NSHostingView(rootView: content)
        hosting.translatesAutoresizingMaskIntoConstraints = false
        scrollView.documentView = hosting

        // The hosting view sizes itself to its intrinsic width (as wide as the
        // SwiftUI HStack of band columns), taller than the scrollview's clip
        // view means vertical scroll, but we set height equal to clip so no
        // vertical scroll happens.
        NSLayoutConstraint.activate([
            hosting.topAnchor.constraint(equalTo: scrollView.topAnchor),
            hosting.leadingAnchor.constraint(equalTo: scrollView.leadingAnchor),
            // Match scrollview height so vertical scroll never engages —
            // horizontal is the only axis we care about here.
            hosting.heightAnchor.constraint(equalTo: scrollView.heightAnchor,
                                            constant: -scrollerHeight()),
        ])
        return scrollView
    }

    func updateNSView(_ scrollView: NSScrollView, context: Context) {
        (scrollView.documentView as? NSHostingView<Content>)?.rootView = content
    }

    /// Reserve the height of the horizontal scroller so the content doesn't
    /// get clipped by it.
    private func scrollerHeight() -> CGFloat {
        NSScroller.scrollerWidth(for: .regular, scrollerStyle: .legacy)
    }
}
