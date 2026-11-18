// swift-tools-version: 5.9
import Foundation
import PackageDescription

let packageDir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let xcframeworkRelPath = "../../dist/ios/FeatherDartVM.xcframework"
let xcframeworkURL = packageDir.appendingPathComponent(xcframeworkRelPath)
let hostDistURL = packageDir.appendingPathComponent("../../dist/host").standardizedFileURL

let hasXCFramework = FileManager.default.fileExists(atPath: xcframeworkURL.path)
let hasHostDylib =
    FileManager.default.fileExists(
        atPath: hostDistURL.appendingPathComponent("libfeather_dart_jit.dylib").path
    ) ||
    FileManager.default.fileExists(
        atPath: hostDistURL.appendingPathComponent("libfeather_dart_jit.so").path
    )

var targets: [Target] = []

if hasXCFramework {
    targets.append(
        .binaryTarget(
            name: "CFeatherDart",
            path: xcframeworkRelPath
        )
    )
} else if hasHostDylib {
    targets.append(
        .target(
            name: "CFeatherDart",
            path: "Sources/CFeatherDart",
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("../../../../vm/include"),
                .define("FEATHER_DART_LINKED_HOST_VM", to: "1"),
            ],
            linkerSettings: [
                .unsafeFlags([
                    "-L\(hostDistURL.path)",
                    "-lfeather_dart_jit",
                    "-Xlinker", "-rpath",
                    "-Xlinker", hostDistURL.path,
                ]),
            ]
        )
    )
} else {
    // Fresh-clone fallback: compiles stub C symbols so `swift build` and
    // `swift test` succeed prior to running `tools/build_vm.py`.
    targets.append(
        .target(
            name: "CFeatherDart",
            path: "Sources/CFeatherDart",
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("../../../../vm/include"),
            ]
        )
    )
}

targets.append(
    .target(
        name: "FeatherDart",
        dependencies: ["CFeatherDart"],
        path: "Sources/FeatherDart"
    )
)

targets.append(
    .testTarget(
        name: "FeatherDartTests",
        dependencies: ["FeatherDart"],
        path: "Tests/FeatherDartTests"
    )
)

let package = Package(
    name: "FeatherDart",
    platforms: [
        .iOS(.v15),
        .macOS(.v13),
    ],
    products: [
        .library(
            name: "FeatherDart",
            targets: ["FeatherDart"]
        ),
    ],
    targets: targets
)
