// The same C SDK, imported directly through the CHarness module.
import CHarness
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif

let provider: ah_complete_fn = { _, _, _, _, _, _, response, _ in
    response?.pointee = strdup("{\"status\":\"completed\",\"output\":[]}")
    return response?.pointee == nil ? AH_IO : AH_OK
}

var config = ah_sdk_config()
ah_sdk_config_init(&config)
config.engine.provider.complete = provider
var sdk: OpaquePointer?
var error = ah_error()
let opened = ":memory:".withCString { path in
    config.engine.database_path = path
    return ah_sdk_open(&config, &sdk, &error)
}
precondition(opened == AH_OK)
defer { ah_sdk_close(sdk) }
let engine = ah_sdk_engine(sdk)
var id = [CChar](repeating: 0, count: Int(AH_ID_CAP))
precondition(ah_session_create(engine, "offline", "Be concise.", &id, &error) == AH_OK)
precondition(ah_submit(engine, id, "first", "Hello", &error) == AH_OK)
precondition(ah_run(engine, id, &error) == AH_OK)
var state = AH_IDLE
precondition(ah_session_state(engine, id, &state, &error) == AH_OK)
precondition(state == AH_COMPLETED)
print(String(cString: ah_state_name(state)))
