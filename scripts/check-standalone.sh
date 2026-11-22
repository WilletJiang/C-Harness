#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
# Build only the extractable project; no parent paths, Swift or AppKit.
mkdir "$work/source"
(cd "$root" && tar --exclude='./build*' --exclude='./.git' -cf - .) | (cd "$work/source" && tar xf -)
cmake -S "$work/source" -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$work/install"
cmake --build "$work/build" --parallel 4
ctest --test-dir "$work/build" --output-on-failure
cmake --install "$work/build"
python3 -B "$work/source/tests/test_flow.py" "$work/install/bin/ah"
mkdir "$work/client"
cp "$work/source/examples/embed.c" "$work/client/main.c"
cp "$work/source/examples/embed.c" "$work/client/main.cpp"
cp "$work/source/examples/durable.c" "$work/client/durable.c"
cat > "$work/client/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(external_client C CXX)
find_package(ah CONFIG REQUIRED)
add_executable(client main.c)
target_link_libraries(client PRIVATE ah::ah)
add_executable(client_cpp main.cpp)
target_link_libraries(client_cpp PRIVATE ah::ah)
add_executable(durable_client durable.c)
target_link_libraries(durable_client PRIVATE ah::ah)
CMAKE
cmake -S "$work/client" -B "$work/client-build" -DCMAKE_PREFIX_PATH="$work/install"
cmake --build "$work/client-build"
"$work/client-build/client"

"$work/client-build/client_cpp"
"$work/client-build/durable_client"

# Embedding as a subproject must not build a CLI or require Python, even when
# the application's own BUILD_TESTING is enabled.
mkdir "$work/nested"
cp "$work/source/examples/embed.c" "$work/nested/main.c"
cat > "$work/nested/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(nested_client C)
include(CTest)
add_subdirectory("${HARNESS_SOURCE}" harness)
if(TARGET ah_cli OR TARGET ah_app_signals OR TARGET ah_embed OR AH_TESTS_ENABLED)
  message(FATAL_ERROR "SDK embedding enabled unrelated products")
endif()
add_executable(nested main.c)
target_link_libraries(nested PRIVATE ah::ah)
CMAKE
cmake -S "$work/nested" -B "$work/nested-build" \
  -DHARNESS_SOURCE="$work/source" -DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE
cmake --build "$work/nested-build" --parallel 4
"$work/nested-build/nested"

if [ "$(uname -s)" = Darwin ] && command -v swift >/dev/null 2>&1; then
  mkdir -p "$work/swift-client/Sources/Client"
  cp "$work/source/examples/embed.swift" "$work/swift-client/Sources/Client/main.swift"
  cat > "$work/swift-client/Package.swift" <<'SWIFT'
// swift-tools-version: 5.9
import PackageDescription
let package = Package(name: "SDKClient", platforms: [.macOS(.v13)],
    dependencies: [.package(name: "CHarness", path: "../source")],
    targets: [.executableTarget(name: "Client",
        dependencies: [.product(name: "CHarness", package: "CHarness")])])
SWIFT
  cat >> "$work/swift-client/Sources/Client/main.swift" <<'SWIFT'
var op = [CChar](repeating: 0, count: Int(AH_ID_CAP))
precondition(ah_session_create(engine, "offline", "", &id, &error) == AH_OK)
precondition(ah_work_start(engine, id, "swift", "flow", #"{"script":"return 6 * 7"}"#, &op, &error) == AH_OK)
precondition(ah_work_run(engine, id, &error) == AH_OK)
var snapshot: UnsafeMutablePointer<CChar>?
precondition(ah_work_json(engine, id, op, &snapshot, &error) == AH_OK)
precondition(String(cString: snapshot!).contains(#""result":42"#))
ah_free(snapshot)
SWIFT
  swift build --package-path "$work/source" --scratch-path "$work/vm-build" --product ah-flow-vm
  swift build --package-path "$work/swift-client" --scratch-path "$work/swift-build"
  vm_bin=$(swift build --package-path "$work/source" --scratch-path "$work/vm-build" --show-bin-path)
  client_bin=$(swift build --package-path "$work/swift-client" --scratch-path "$work/swift-build" --show-bin-path)
  cp "$vm_bin/ah-flow-vm" "$client_bin/"
  "$client_bin/Client"
fi
