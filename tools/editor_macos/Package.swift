// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "EODModdingSuite",
    platforms: [
        .macOS(.v14)
    ],
    products: [
        .executable(name: "EODModdingSuite", targets: ["EODModdingSuite"])
    ],
    targets: [
        .executableTarget(
            name: "EODModdingSuite",
            path: "Sources/EODModdingSuite"
        )
    ]
)
