# Experimental extension host

CED 0.2.0 development implements phases 0 and 1 of the [architecture](arquitetura-extensoes.md). The [authoring specification](extension-authoring-spec.md) defines the draft wire shapes; this document identifies what the current host actually supports. Negotiate `hostMethods` rather than assuming every draft method exists.

## Available contributions and services

- Manifest format 1; exact `MAJOR.MINOR.PATCH` versions; API major 1, negotiated version `1.0`.
- Executable packages with platform entrypoints (`darwin-arm64`, `darwin-x86_64`, `linux-arm64`, `linux-x86_64`), declared `onCommand:<own-command-id>` activation and `documents.read`/`documents.edit` capabilities.
- Command titles/categories and AND conditions: `hasDocument`, `editorTextFocus`, `hasSelection`, `workspaceTrusted`, `languageId`.
- Declarative configuration schemas with string/boolean/number defaults and user/workspace scope can be validated and installed. Effective configuration consumption/overrides are deferred; `configuration/get` is not advertised. Schema enums and numeric ranges are unsupported.
- Lifecycle: `ced/initialize`, `ced/activate`, `commands/invoke`, `ced/deactivate`, `ced/cancel`.
- Host methods: `editor/getContext`, `documents/list`, `documents/get`, `documents/applyEdits`, `window/showMessage`, `log/write`.
- Configuration/state/events return `UnsupportedCapability`; unknown methods return `MethodNotFound`. Unsupported contribution keys are rejected during installation.

All offsets are UTF-8 bytes, not character indexes or UTF-16 units. Documents have session-local opaque IDs and monotonic revisions. Real edits, native input, completion, undo and redo update the revision; saving, renaming and tab reordering preserve it. A prepared extension transaction validates every range, UTF-8 boundary, insertion and final selection before committing once through the private CUI adapter. The complete success envelope and an outgoing queue slot are reserved before commit, in addition to undo allocation. Allocation failure or queue saturation cannot commit an edit with a partial reply. Transport loss after a confirmed commit still has an unknown outcome and never triggers replay. No-op extension edits preserve the revision, selection and history. An initialized inactive tab can be edited without stealing focus; a tab that has never initialized its editor state returns `Unavailable`.

Requests include the initialization context. Stale session/workspace/generation, document revision and optional selection revision are checked before a commit. A workspace switch invalidates requests before closing documents. Extension text edits run at the start of a UI frame, before rendering consumes buffer pointers. Document reads include unsaved text; API edits never save to disk. Legacy non-UTF-8 buffers remain open but text access returns `Unavailable`.

## Installation and trust

Use the command palette's **Install Extension from Folder** and **Manage Extensions** actions. Review installation is explicit. Installation, hashing, profile writes, package validation and process I/O run on workers. Filesystem packages are copied without installation scripts; their manifests are parsed from the exact bytes included in the reviewed digest. A SHA-256 digest identifies contents; it does not authenticate a publisher.

The profile is `~/.ced`. Versioned packages live at `extensions/<id>/<version>/`, active versions/grants/enabled state and canonical trusted workspace paths at `extensions.json`, and advisory locks at `extension-locks/`. Profile updates use a temporary file, fsync and atomic rename under a nonblocking cross-window lock. A shared version lease prevents uninstalling a package used by another window. Old packages are retained on update; uninstall acquires leases for all versions (up to 128) before removing the registration and packages. Reinstall a retained package folder to roll back, with a fresh review.

Packages reject links, traversal, special files, unavailable entrypoints, unknown capabilities/contributions, duplicate JSON keys and incompatible identities/API requirements. A registered version cannot be replaced with different contents. Installed contents are revalidated before starting a process. A damaged catalog fails closed; the original profile is preserved for diagnosis.

Workspace trust is never read from repository configuration. Saved documents inside the canonical trusted workspace are authorized by default according to granted capabilities. The manager can grant an extension access to the active untitled/outside document for the current session. Document grants remain tied to that document ID and disappear on manager reload/workspace change; revoking one does not revoke the default workspace access policy. Revoking an explicit document grant stops the affected instance and cancels its pending invocation. Save As/rename also stop an instance when the document loses its authorized scope; closing a document cancels its pending command and removes its explicit grants. Subsequent activation is lazy and starts a fresh process session. Disabling extensions and revoking trust stop existing processes. A failed process requires explicit restart; no command or edit is replayed.

Executables run with the user's operating-system powers, without a sandbox. They receive an absolute executable argv, the installed package as working directory, and only `HOME`, `TMPDIR`, `LANG`, `LC_ALL`, `LC_CTYPE` from the host environment when present. `PATH` and credential variables are not forwarded; use a native executable or an absolute interpreter in a shebang. Runtime requirements are displayed, never installed automatically. Workspace identity comes from initialization, not the process working directory.

## Operational limits

| Resource | Current limit |
| --- | --- |
| Installed / simultaneously running extensions | 8 / 4 per window |
| Package size / entries / directory depth | 64 MiB / 2,000 / 32 |
| Manifest / commands / configuration keys | 256 KiB / 100 / 100 |
| Document / edits per atomic transaction | 2 MiB / 1,000 |
| JSON depth / structural tokens | 64 / 32,768 (manifest depth 32) |
| Header / message body (each direction) | 1 KiB / 8 MiB |
| Command response, including JSON-RPC envelope | 64 KiB |
| Incoming/outgoing queues | 8 messages and 8 MiB of conservatively charged allocations per direction |
| Shared transport, parser and queued-allocation budget | 64 MiB per host process |
| UI service dispatch | At most 4 messages per instance per frame |
| UI dispatch scheduling | Soft aggregate budget of 2 ms, rotating between instances; excess work is deferred |
| Initialize / activate / command timeout | 3 s / 2 s / 5 s |
| Graceful stop / SIGTERM / SIGKILL | Deactivate immediately, TERM after 500 ms, KILL after 1 s |
| User message / rate | 512 bytes / 5 per 10 seconds per instance |
| Log message / rate / rotation | 8 KiB / 64 KiB per second / three 1 MiB files |

Framing accepts one ASCII `Content-Length: <positive-decimal>\r\n\r\n` header and UTF-8 JSON, without a BOM or NUL. Batched messages, numeric/null request IDs, duplicate keys and numbers that overflow to nonfinite values are rejected. Unknown optional RPC fields are ignored; known required fields and mutually exclusive result/error shapes remain validated. Manifest unknown-field validation stays strict. Extension request IDs use increasing `ext:<counter>` values; host IDs use `ced:<counter>`. Responses retain the request ID. Stale document errors include `data.currentVersion`; malformed document IDs/parameters return `InvalidParams` (`-32602`), while closed/unknown well-formed IDs return `Unavailable`. Partial/coalesced reads, partial writes, `EAGAIN`, EOF and broken pipes are handled by the worker. Traffic or allocation overflow terminates the affected instance. Advertised limits can be lower than the draft ceilings; the structural-token ceiling also applies.

Invalid notifications are diagnosed without replies. Diagnostics are rate-limited; eight violations in ten seconds stop the offending instance. The frame budget is cooperative: an individual atomic service can exceed it, but subsequent messages are deferred, and lifecycle deadlines/reaping are still checked for every instance.

The supervisor drains/rate-limits stderr independently and writes logs under `logs/extensions/<session>/<id>.log`, replacing terminal control bytes. Lifecycle request IDs, methods, durations/error codes and confirmed edit revisions are logged without document payloads. Pending records are appended in a bounded buffer rather than replacing each other on every call. The manager displays source, grants, activation time, pending request count and queued bytes. The UI's **Open Log** opens that file as a normal document. Stopping cancels pending work, sends deactivation and ultimately kills the process group and reaps the direct child. Normal frames do not wait for shutdown. Application shutdown has a bounded process wait; a worker delayed in filesystem I/O detaches with ownership of its own resources. Catalog workers also release UI ownership on shutdown. Interrupted installation can leave an unregistered staging directory, but cannot activate a partially written package/profile.

There is no live cross-window profile watcher yet. Other windows pick up catalog/trust changes when their manager reloads, such as after a workspace switch, a management operation or application restart. Current-window revocation invalidates API access immediately.

## Deferred phases

API 1.0 is experimental until the stable-operation phase is accepted. Persistent extension state, effective configuration/overrides, subscriptions/events, contributed keybindings/menus, status/output/tree views, themes, diagnostics contributions and multiple extension language servers are not implemented. There is no marketplace, automatic update, embedded scripting runtime or public CUI ABI.

Run `make test` for the native integration, failure, protocol, edit and UI suites. `make EXTENSIONS=OFF BUILD_DIR=build-no-extensions test` verifies the editor without the subsystem. Tests do not install anything in the real user profile.
