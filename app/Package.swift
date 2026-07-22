// swift-tools-version:5.9
// HonestEQ macOS app — SwiftUI + SwiftPM.
// Build: swift build -c release --arch arm64 --arch x86_64
// (or use ../app/build.sh which also assembles the .app bundle)

import PackageDescription

let package = Package(
    name: "HonestEQ",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "HonestEQ", targets: ["HonestEQ"]),
    ],
    targets: [
        .executableTarget(
            name: "HonestEQ",
            path: "Sources/HonestEQ"
        ),
    ]
)
