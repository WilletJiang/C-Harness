// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "CHarness",
    platforms: [.macOS(.v13)],
    products: [.library(name: "CHarness", targets: ["CHarness"])],
    targets: [
        .target(
            name: "CHarness",
            path: ".",
            exclude: ["src/cli"],
            sources: ["src", "vendor/yyjson/yyjson.c", "vendor/tomlc17/tomlc17.c"],
            publicHeadersPath: "include",
            cSettings: [
                .headerSearchPath("src"),
                .headerSearchPath("vendor/yyjson"),
                .headerSearchPath("vendor/tomlc17"),
                .define("_POSIX_C_SOURCE", to: "200809L"),
                .define("_GNU_SOURCE", to: "1"),
                .define("_DARWIN_C_SOURCE", to: "1"),
            ],
            linkerSettings: [
                .linkedLibrary("curl"), .linkedLibrary("sqlite3"),
                .linkedLibrary("pthread"), .linkedLibrary("m"),
                .linkedFramework("CoreFoundation", .when(platforms: [.macOS])),
            ]
        ),
    ],
    cLanguageStandard: .c17
)
