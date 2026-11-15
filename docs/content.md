# Typed input, tool results, artifacts, and retention

`ah/content.h` adds bounded Responses content arrays alongside the existing
`ah_submit` text API. No upload, URL fetch, model call, or deletion happens merely
because an artifact is imported or listed. A model request expands only artifacts
named in active user input or native tool output parts.

```c
ah_submit_content(engine, session, "image-1",
    "[{\"type\":\"input_text\",\"text\":\"Describe this image\"},"
    "{\"type\":\"input_image\",\"image_url\":\"https://example.com/image.png\"}]",
    &error);
```

Accepted parts follow the native [Responses image inputs](https://developers.openai.com/api/docs/guides/images-vision)
and [file inputs](https://developers.openai.com/api/docs/guides/file-inputs) shapes:

| Type | Required source | Optional fields |
| --- | --- | --- |
| `input_text` | Nonempty `text` | None |
| `input_image` | Exactly one `image_url`, `file_id`, or local `artifact_id` | `detail`: `auto`, `low`, `high`, `original` |
| `input_file` | Exactly one `file_url`, `file_id`, `file_data`, or local `artifact_id` | `filename`; `detail`: `auto`, `low`, `high` |

`image_url` accepts HTTP(S) or an explicit MIME base64 data URL. `file_data`
accepts a MIME base64 data URL, or padded base64 with a recognized filename
extension. Local file data and artifact references require a filename. URLs
containing credentials, unknown fields, duplicate keys, invalid UTF-8, embedded
NUL, ambiguous sources, and malformed base64 are rejected. Native remote URLs
and file IDs are syntax checked; their existence and content are verified by the
provider, not downloaded by this library.

An input array has at most 64 parts and must fit `max_context_bytes`. Each local
artifact has an 8 MiB hard limit. The complete expanded model request must also
fit `max_context_bytes`; importing a large file does not enlarge that budget.
The CLI exposes `--context-bytes N` to raise its default 1 MiB request limit,
up to 64 MiB. RPC input frames remain limited to 1 MiB; larger local files can
be imported by path and selected by reference.

## Native tool results

The public `ah_tool_result.format` field explicitly selects the result shape.
Zero-initialized callbacks use `AH_TOOL_RESULT_JSON`: `json` retains the legacy
arbitrary JSON payload and the model receives the existing `is_error`/`result`
string envelope. A JSON object with a `content` key, or an array that resembles
native parts, never changes this behavior by itself.

Set `AH_TOOL_RESULT_CONTENT` to return the same native part array accepted by
`ah_submit_content`. The callback still returns a malloc-owned JSON string;
the runtime frees it. For example, a callback can return:

```c
result->json = strdup(
    "[{\"type\":\"input_text\",\"text\":\"Latest chart\"},"
    "{\"type\":\"input_image\",\"image_url\":\"https://example.com/chart.png\"}]");
result->format = AH_TOOL_RESULT_CONTENT;
return result->json ? AH_OK : AH_IO;
```

The model receives an array in `function_call_output.output`, with an initial
`input_text` part recording `is_error`, followed by the native content. This uses
[Responses native function outputs](https://developers.openai.com/api/docs/guides/function-calling).
Images and files are actual provider input parts, not JSON descriptions of media.
Audio, video, unsupported MIME types, ambiguous media sources and malformed
content are rejected. A malformed READ result becomes a settled JSON error;
a malformed effect result requires explicit outcome resolution.

Worker callbacks only return content. The owner validates it and stores inline
binary data as immutable artifacts in the same transaction as the durable result.
Completed operations can be captured out of order; model-visible receipts remain
in call order. Captured native results and their format survive restart without
re-executing the completed operation. A failed capture transaction leaves no
partial artifacts or session references.

`max_output_bytes` bounds the callback's complete serialized result, including
inline base64. `max_model_tool_bytes` bounds the stored model view's text and
artifact references; base64 expansion is governed by `max_context_bytes` for the
complete model request. A result can therefore retain more text than fits in a
single request. The receipt shortens text at UTF-8 boundaries, retains images and
files, and identifies `read_result` for retrieval. If the media references alone
cannot fit the model view, the receipt explicitly reports `media_deferred` and
returns a retrieval reference instead. Oversized expanded requests fail with
`AH_LIMIT`; binary data is never silently truncated.

`read_result` without `offset` or `limit` returns a saved native array as native
content, including after a fork. Supplying either pagination field requests the
existing UTF-8 JSON text page API, which exposes canonical artifact references
and `next_offset`. Thus callers can page long text and later retrieve the native
media. Retrieval respects the current output and request budgets; raising a
budget does not rewrite already published receipts. Session result-reference and
artifact-reference checks apply to every retrieval and expansion.

## Immutable local artifacts

`ah_artifact_import(engine, session, path, mime, id, error)` reads a regular file;
the final path component cannot be a symlink. The supported MIME values are
`image/png`, `image/jpeg`, `image/gif`, `image/webp`, `application/pdf`,
`text/plain`, and `application/json`. Binary signatures or UTF-8/JSON are checked;
the library does not implement full image or PDF parsers. Imports check for file
changes while reading and store an immutable snapshot in SQLite.

The returned 64-character identifier is SHA-256 of MIME, a NUL separator, and
the bytes. Identical representations share one blob. Inline data is normalized
the same way in the submission transaction, so journal events, command records,
and items contain references rather than repeated base64. Command identity uses
the canonical typed input and distinguishes typed submissions from plain text.

Artifact references belong to a session. Knowing another session's identifier
does not grant access. Forks inherit references; compaction leaves them available
for explicit later selection. `ah_artifacts_json` lists only the current
session's metadata and never includes file bytes.

```sh
ah artifact-import --session SESSION --artifact-path ./figure.png --mime image/png
ah submit-content --session SESSION --command-id figure-1 \
  --content '[{"type":"input_image","artifact_id":"SHA256"}]'
ah artifacts --session SESSION
```

## Permanent deletion

Compaction preserves history. `ah_forget` permanently deletes the selected
session's audit history and resources; it is not an archive or export. It runs
only when the engine is idle, sessions are idle/completed or stopped/failed,
operations are settled, and no steering or agent messages are pending. Unknown
or in-progress task verification must be reconciled first; task acceptance is
not required to deliberately discard abandoned work. A stopped agent's resume
marker alone does not block deletion, and forgetting never resumes it. An agent
tree must be selected explicitly from its root.

Surviving sessions that reference the selected session's tool receipts or context
checkpoints prevent deletion with `AH_BUSY`. Forget dependent forks first.
Shared artifacts remain until their last session reference is deleted.
Foreign-key failures roll back the entire deletion, including for unknown future
tables. The library never infers that history is disposable because it is old.

```sh
ah forget --session SESSION
ah forget --session ROOT --forget-tree
ah vacuum
```

Deletion reclaims logical rows. `ah_storage_vacuum` / `ah vacuum` explicitly
rewrites SQLite to reclaim file space; no automatic vacuum or additional history
deletion occurs. Neither operation is a secure erase guarantee for storage media
or external backups.

RPC methods are `submit_content` (`content` array and `command`),
`artifact_import` (`path`, `mime`), `artifacts`, `forget` (`tree` boolean), and
`storage_vacuum`. All except `storage_vacuum` require `session`.
