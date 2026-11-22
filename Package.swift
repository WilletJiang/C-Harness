// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "CHarness",
    platforms: [.macOS(.v13)],
    products: [.library(name: "CHarness", targets: ["CHarness"]),
               .executable(name: "ah-flow-vm", targets: ["FlowVM"])],
    targets: [
        .target(name: "FlowQuickJS", path: "vendor/quickjs",
                sources: ["quickjs.c", "cutils.c", "libunicode.c", "libregexp.c", "dtoa.c"],
                publicHeadersPath: "include",
                cSettings: [.define("_GNU_SOURCE"), .define("asm", to: "__asm__"), .define("CONFIG_VERSION", to: "\"2026-06-04\"")]),
        .executableTarget(name: "FlowVM", dependencies: ["FlowQuickJS"], path: "packages/codemode",
                          sources: ["main.c"]),
        .target(
            name: "CHarness",
            path: ".",
            sources: ["packages/agent/src", "vendor/yyjson/yyjson.c", "vendor/tomlc17/tomlc17.c"],
            publicHeadersPath: "packages/agent/include",
            cSettings: [
                .headerSearchPath("packages/agent/src"),
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
