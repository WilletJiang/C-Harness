# Durable tools and host work

`ah/work.h` adds resumable execution to the existing tool executor. A tool opts in
with `ah_tool.work`; ordinary tools retain their terminal callback contract.
Tool policy, argument validation, approval, workspace ownership, cancellation,
and session/task budgets still apply to every callback attempt.

An operation has one durable identity and one status/result in `operations`.
Checkpoints, waits, generations, keyed child relationships, and signal inboxes
extend that record. They do not create a second execution engine or a model
context fork.

## Callback contract

Register a positive schema version and an explicit replay policy:

```c
static const ah_work_config work = {
    .version = 1,
    .replay = AH_REPLAY_SAFE,
    .max_restarts = 5,
};

ah_tool tool = {
    .name = "await_ready",
    .description = "Wait for a host readiness signal",
    .parameters_json = "{\"type\":\"object\",\"properties\":{},"
                       "\"additionalProperties\":false}",
    .effect = AH_EFFECT_READ,
    .execute = await_ready,
    .work = &work,
};
```

The runtime copies the configuration at registration. The existing host-owned
tool context must still outlive the engine. Each invocation receives:

- `checkpoint_json`: the last committed JSON object, or `{}` initially.
- `wake_json`: the claimed signal/timer/child snapshot, or NULL initially.
- `generation`: the durable invocation generation.

A terminal callback fills `result->json` as before. A callback that wants to
continue instead fills malloc-owned `result->continuation_json` and returns
`AH_OK`. The runtime owns and frees both result buffers. A continuation has this
shape:

```json
{
  "checkpoint": {"phase": "awaiting_review"},
  "wait": {"signal": "review", "until_ms": 1900000000000}
}
```

`checkpoint` is required and must be an object. `wait` is optional: omitting it
continues at another charged callback attempt. A wait must name a signal,
absolute Unix time in milliseconds, or child keys. When several conditions are
present, any satisfied condition can wake the operation. A ready signal takes
precedence over a ready child policy, which takes precedence over the timer.

For example, a two-phase callback can park without holding a worker:

```c
static ah_code await_ready(void *context, const ah_tool_call *call,
                           ah_tool_result *result, ah_error *error) {
    (void)context;
    (void)error;
    if (!strcmp(call->checkpoint_json, "{}"))
        result->continuation_json = strdup(
            "{\"checkpoint\":{\"phase\":\"waiting\"},"
            "\"wait\":{\"signal\":\"ready\"}}");
    else
        result->json = strdup(call->wake_json);
    return result->continuation_json || result->json ? AH_OK : AH_IO;
}
```

The continuation, checkpoint, wait, and new child intents commit together. A
crash before that commit leaves the previous checkpoint in force. Callback stack
frames and local variables are never saved. A semantically changed checkpoint
resets the consecutive interrupted-attempt counter; committing the same JSON
state does not reset it. Normal continuation steps remain subject to the
ordinary tool-call budget.

## Keyed child calls

A continuation can schedule registered tools and wait for their results:

```json
{
  "checkpoint": {"phase": "collect"},
  "calls": [
    {"key": "left", "tool": "fetch", "arguments": {"item": 1}},
    {"key": "right", "tool": "fetch", "arguments": {"item": 2}}
  ],
  "wait": {"calls": ["left", "right"], "policy": "all"}
}
```

Child identity is `(parent operation ID, key)`, across every continuation and
restart. Repeating the same key, tool name, and structurally equal JSON arguments
returns the original child; object key order does not matter. A conflicting
reuse is rejected. A child can be an ordinary tool or another durable tool.
There are at most 32 direct children, eight nesting edges, and 128 operations
including the root in one work tree.

The join policy is:

| Policy | Wake condition | Remaining children |
| --- | --- | --- |
| `all` (default) | Every selected child has a terminal receipt | All selected results available |
| `any` | A selected child has a terminal receipt, including an error | Remain owned by the parent |
| `fail_fast` | A selected child fails, or all selected children finish | On failure, unfinished descendants are cancelled before the parent resumes |

An uncertain effect is not a terminal failure receipt. It requires explicit
resolution before the enclosing work can finish. A parent returning a terminal
result cancels its unfinished descendants; active callbacks must drain, and
unknown effects remain unresolved. `any` therefore permits examining one result
and continuing to wait without abandoning the other children.

A wake contains a `calls` object keyed by child key. Each value includes
`operation_id`, `name`, `status`, `result`, `result_format`, and `is_error`.
Pending children have a NULL result. Host and nested operations never append
synthetic function calls or outputs to model history. The outer model-requested
operation still publishes its single terminal result.

## Signals, timers, and cancellation

`ah_signal(engine, session, operation, key, value_json, &delivery, &error)` is an
owner-thread API. Signals can arrive before the callback starts waiting. The
first value for an operation/key wins durably; repeated deliveries while open
return `AH_SIGNAL_DUPLICATE`. Closed operations and expired waits take precedence
and return `AH_SIGNAL_CLOSED`, including retries of existing keys. An unknown
destination returns `AH_SIGNAL_NOT_FOUND`.
These delivery outcomes are separate from the API's `ah_code`.

Signal consumption and the new invocation generation commit together. A resumed
callback receives the claimed value as
`{"reason":"signal","signal":{"key":"ready","value":...}}`.
Use a new signal key for each later phase; a consumed key cannot be waited on
again. Signal keys are at most 128 bytes, values at most 16 KiB, with at most 128
keys per operation. Continuation envelopes are bounded at 64 KiB.

Timers use Unix wall-clock milliseconds so they survive process downtime.
`ah_work_next_wake` returns the earliest parked timer, or zero if none exists.
The host invokes the run API again after a delivery or when that time is due.
Parked work returns control to the host; it does not occupy a worker while idle.

`ah_work_cancel` cancels a quiescent operation and its descendants. It settles
unstarted or parked work with error receipts and preserves trustworthy results.
An executing external effect without a trustworthy receipt remains unknown.
While execution is active, `ah_cancel` and `ah_cancel_job` retain their existing
cross-thread cancellation roles. Cancellation requests do not certify that an
external effect was rolled back.

## Host-created work

Create a fresh session, then call `ah_work_start` with a host key, tool name, and
JSON arguments. The first start permanently marks that session as host-only;
sessions with submitted input, operations, a task, agent membership, or model history
cannot be converted. Root keys are scoped to the host session and follow the
same immutable identity rule as child keys.

`ah_work_run` advances the common executor to completion or a boundary such as a
durable wait, approval, input, cancellation, deadline, or uncertain outcome.
It never invokes a provider. Calling `ah_run` on a host-only session follows the
same path. Approvals and unknown effects use `ah_decide` and `ah_resolve` as for
ordinary tools. `ah_work_json` returns a malloc-owned operation snapshot,
continuation metadata when present, and direct child snapshots.

The complete offline [C example](../examples/durable.c) registers a durable tool,
parks it, delivers a host signal, and prints its completed snapshot. It includes
a provider guard that fails if model execution is attempted.

## Independent observation

`ah/observe.h` supplies a separate read-only connection for a UI or service that
observes an active writer. `ah_reader_open` supports an existing schema-8 database
file managed by this runtime with its DELETE journal mode. It does not create
the database, run harness migration or recovery, or take the engine ownership
lock. Externally converted WAL databases are outside this contract. Each reader
belongs to one thread.

`ah_reader_session_json` returns a consistent session snapshot plus `cursor`, the
last global event sequence in the same read transaction. Start event reads after
that cursor to avoid a snapshot/subscription gap. `ah_reader_events` reads after a durable sequence cursor and
copies up to 256 events and 4 MiB before invoking observers. SQLite read locks
are released before callbacks, so a slow UI does not hold the writer's commit
lock. Save the returned `last_seq` and use it as the next call's `after_seq`;
passing a NULL session includes all sessions. Callbacks borrow event data and
must not close or reenter their reader.

An oversized first event returns `AH_LIMIT` without advancing the cursor. A later
oversized event ends the batch after the preceding events. The host must handle
that explicit size boundary; retrying the same oversized event cannot advance.
This API provides snapshots and event batches; the host owns polling and UI
transport.

## Recovery and version changes

`AH_REPLAY_NEVER` preserves the ordinary conservative interruption behavior.
`AH_REPLAY_SAFE` authorizes re-entry from the last committed checkpoint after
an interrupted callback, bounded by `max_restarts` (zero selects five). It is a
host promise about the callback, not an exactly-once guarantee. External systems
need their own idempotency keys or reconciliation when a process can fail
between an external effect and the local checkpoint commit.

On a tool version change, a pure `migrate` callback may transform the saved
checkpoint. It receives the prior version, immutable arguments, and checkpoint,
and returns a malloc-owned JSON object. It must not perform effects. An absent
or failed migration does not silently execute old state under the new version.
Tool policy/schema mismatches, exhausted budgets, invalid continuations, and
unresolved effects remain explicit execution boundaries.
