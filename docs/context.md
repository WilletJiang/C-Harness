# Long-running context

`--auto-context` enables bounded automatic maintenance. The C equivalent is
`ah_context_config_init` followed by `ah_context_enable`. Defaults trigger at
three quarters of the request byte budget, target one half, keep three recent
model exchanges, and reserve one eighth of the budget (at most 64 KiB). These
are byte limits, not estimates of the provider's token window. CLI overrides
are `--context-bytes`, `--context-trigger`, `--context-target`,
`--context-reserve` and `--keep-exchanges`.

## Exact archives

Maintenance runs only at a model boundary with settled tool operations. It moves
older complete model/tool exchanges out of the active request and records a
checkpoint referencing their original journal rows. It never executes tools or
makes an extra model request. User messages, human `ask`/`wait` replies, loaded
skills and the latest native compaction state remain present. A call and its
result are never split. Exchange identities are stored with items in schema 6.

The replacement note explicitly says that earlier work is archived and supplies
a checkpoint ID. `recall` lists accessible checkpoints or pages an exact archived
item, with bounded UTF-8 offsets and continuation cursors. Full tool results also
remain available through `read_result`. A fork inherits access; a fresh subagent
does not. The checkpoint note and recall tool remain available after reopening
even when further automatic maintenance is disabled.

This fallback is structural retrieval, not a generated semantic summary. It does
not guarantee that the model will remember every archived fact without looking it
up. Independent task acceptance checks remain necessary. If protected inputs and
recent exchanges cannot fit with the configured reserve, the runtime returns
`AH_LIMIT`; the host must provide an explicit checkpoint or change the budget.
It does not silently truncate user constraints or produce an incomplete request.

## Native provider compaction

`--compact-tokens N` explicitly declares support for Responses server-side
compaction and sends `context_management` with that token threshold. The matching
C field is `compact_threshold_tokens`. The runtime does not infer this capability
from a model name, and no provider configuration is enabled by default.

A validated successful response may contain an encrypted `compaction` item.
Acceptance stores it atomically, archives the already-settled prior prefix, and
preserves the current response's tool calls and exact user/human inputs. Only the
newest opaque compaction state is needed after later native compaction. Partial
streams or invalid encrypted items cannot prune history. The provider still owns
the semantic quality and token accounting of that compression; fixture validation
does not establish availability on a particular subscription route.

The [provider guide](https://developers.openai.com/api/docs/guides/compaction)
describes stateless chaining and encrypted compaction items. The C request retains
the full reconstructible local journal; it does not depend on a socket response ID.

## Stable skill placement

A newly loaded skill is appended at the next settled model boundary. Existing
input items keep their order and contents. Repeated unchanged loads reuse the same
item; references are not inserted ahead of all earlier history. Reloading changed
instructions and explicit unloading deactivate the obsolete snapshot. Manual or
native compaction can deactivate a loaded item; its pinned snapshot is restored
when needed. Disabling skills omits their managed items without deleting the pin.

The existing request prefix is preserved when merely adding a skill, verified by
input-array equality tests. Reload/unload and compaction necessarily change some
context; no cache-hit guarantee is implied. The [cache contract](https://developers.openai.com/api/docs/guides/prompt-caching)
requires a matching rendered prefix and compatible request settings.

## Persistence and cleanup

Checkpoints reference journal items instead of duplicating a whole transcript.
Transactions cover archive membership, visibility and the checkpoint event.
Original full receipts, command IDs and result permissions remain durable.
Compaction reduces model input, not the database's retained audit history.
Use explicit [retention](content.md) operations to delete quiescent sessions and
reclaim storage; surviving forks prevent deletion of referenced checkpoints.
