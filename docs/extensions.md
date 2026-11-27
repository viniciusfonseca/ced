# Experimental extension host

CED 0.2.0 development implements phases 0 and 1 of the [architecture](arquitetura-extensoes.md). The [authoring specification](extension-authoring-spec.md) defines the draft wire shapes; this document identifies what the current host actually supports. Negotiate `hostMethods` rather than assuming every draft method exists.

## Available contributions and services

- Manifest format 1; exact `MAJOR.MINOR.PATCH` versions; API major 1, negotiated version `1.0`.
- Executable packages with platform entrypoints (`darwin-arm64`, `darwin-x86_64`, `linux-arm64`, `linux-x86_64`, `windows-arm64`, `windows-x86_64`, `windows-i686`), declared `onCommand:<own-command-id>` activation and `documents.read`/`documents.edit` capabilities.
- Command titles/categories and AND conditions: `hasDocument`, `editorTextFocus`, `hasSelection`, `workspaceTrusted`, `languageId`.
- Executable extensions can contribute native tabs to the sidebar and the terminal panel using [tab protocol 1](#native-tab-contributions).
- Declarative configuration schemas with string/boolean/number defaults and user/workspace scope can be validated and installed. Effective configuration consumption/overrides are deferred; `configuration/get` is not advertised. Schema enums and numeric ranges are unsupported.
- Lifecycle: `ced/initialize`, `ced/activate`, `commands/invoke`, `ced/deactivate`, `ced/cancel`.
- Host methods: `editor/getContext`, `documents/list`, `documents/get`, `documents/applyEdits`, `window/showMessage`, `log/write`.
- Configuration/state/events return `UnsupportedCapability`; unknown methods return `MethodNotFound`. Unsupported contribution keys are rejected during installation.

All offsets are UTF-8 bytes, not character indexes or UTF-16 units. Documents have session-local opaque IDs and monotonic revisions. Real edits, native input, completion, undo and redo update the revision; saving, renaming and tab reordering preserve it. A prepared extension transaction validates every range, UTF-8 boundary, insertion and final selection before committing once through the private CUI adapter. The complete success envelope and an outgoing queue slot are reserved before commit, in addition to undo allocation. Allocation failure or queue saturation cannot commit an edit with a partial reply. Transport loss after a confirmed commit still has an unknown outcome and never triggers replay. No-op extension edits preserve the revision, selection and history. An initialized inactive tab can be edited without stealing focus; a tab that has never initialized its editor state returns `Unavailable`.

Requests include the initialization context. Stale session/workspace/generation, document revision and optional selection revision are checked before a commit. A workspace switch invalidates requests before closing documents. Extension text edits run at the start of a UI frame, before rendering consumes buffer pointers. Document reads include unsaved text; API edits never save to disk. Legacy non-UTF-8 buffers remain open but text access returns `Unavailable`.

## Document renderers

Executable extensions with `documents.read` can declare up to eight renderers in
`contributes.documentRenderers`. Each entry has a filename extension, a title
and a command declared by the same package. Use a dot followed by 1–16 lowercase
ASCII letters, digits, hyphens or underscores for the filename extension. CED
matches suffixes case-insensitively. If multiple enabled extensions with read
access claim the same suffix, the first extension ID in lexical order wins.

This manifest fragment registers a renderer for an Agentic Solutions diagram.

```json
{
  "capabilities": ["documents.read"],
  "activationEvents": ["onCommand:example.solutions.render"],
  "contributes": {
    "commands": [{"id": "example.solutions.render", "title": "Render Solution"}],
    "documentRenderers": [
      {"extension": ".asln", "title": "Diagram", "command": "example.solutions.render"}
    ]
  }
}
```

CED advertises `host.documentRenderProtocol: 1` in `ced/initialize`. Opening a
matching text document in a trusted workspace activates its renderer when the
extension has access to the document. Use **Source** to edit the file and the
renderer title to return to the preview. Opening at a specific line shows the
source. An extension command condition can use `hasDocument`, `workspaceTrusted`
and `languageId`. Renderer invocations have an empty selection and no text focus.

The host sends `commands/invoke` with the document's version in
`params.editor.document` and `arguments: [{"action":"render","extension":".asln"}]`.
Fetch the captured document through `documents/get` using its `documentId` and
the request context. This includes unsaved text. Draw the image in the extension
process and return this shape as the command result.

```json
{
  "render": {
    "width": 1,
    "height": 1,
    "rgba": "Nn3S/w==",
    "interactive": false
  }
}
```

`rgba` contains standard padded base64 of `width × height × 4` straight RGBA8
bytes, in row-major order starting at the top left. The edge limit is 1,024
pixels and is advertised as `limits.maxRenderImageEdge`. The JSON-RPC response
limit is advertised as `limits.maxRenderResultBytes`. The default viewer supports
zoom and pan. With `interactive: true` the image is fitted to the pane and CED
sends `{"action":"click","extension":".asln","x":12,"y":34}` in the arguments
array. Coordinates are zero-based image pixels. Return another frame to update
the surface after the click. The editor owns the preview surface and the
extension owns every pixel shown inside it.

The host displays results only for the matching open document revision. The
preview invocations have read access through the normal document grants. When
`host.documentRenderEdits` is true, a click may also call `documents/applyEdits`
with the normal edit grant, the active preview document ID, and the clicked
revision as `expectedVersion`. CED applies the edit to the buffer with undo
history and renders the new revision. Automatic render events have read access.
Ordinary palette invocations still use `arguments: null`.

### Native graph previews

When the host advertises `host.documentGraphProtocol: 3`, a document renderer
can return a `graph` object in its command result. CED draws this graph in its
own UI and processes pointer gestures locally. The `render` response remains
available for extensions that provide an RGBA image.

A graph contains a `nodes` array (up to 64 items) and a `links` array (up to
256 items). A node has `label`, `x`, `y`, `width` and `height` fields. Node
positions are signed integer coordinates from -8,388,607 to 8,388,607. The
initial viewport fits the original 960 by 620 logical surface, whose canvas
begins at y=84. Node sizes range from 40 by 28 to 960 by 620. Each link has `from`, `to`, `style`
(`solid`, `dotted`, `dashed`) and `direction` (`none`, `forward`, `backward`,
`both`). Each endpoint has a zero-based `node` index, a `side` (`top`,
`right`, `bottom`, `left`) and a fractional `offset` between 0 and 1.

With the edit grant, CED sends one `graphEdit` invocation per completed change.
The first argument carries `action: "graphEdit"`, the renderer's `extension`
and an `edit` object. The edit is one of these forms.

```json
{"type":"moveNode","node":0,"x":160,"y":180}
{"type":"addNode","x":250,"y":340}
{"type":"editNode","node":0,"label":"Review","width":170,"height":70}
{"type":"resizeNode","node":0,"x":140,"y":170,"width":190,"height":80}
{"type":"removeElements","nodes":[0,2],"links":[1]}
{"type":"removeNodes","nodes":[0,2]}
{"type":"addLink","link":{"from":{"node":0,"side":"right","offset":0.3},"to":{"node":1,"side":"left","offset":0.7},"style":"dashed","direction":"both"}}
```

Node indices refer to the graph for the document revision in the invocation.
`moveNode` uses the top-left position and `addNode` uses the center of the
new node. `resizeNode` changes the position and size in one edit.
`removeElements` deletes the specified node and link indices, plus any links
attached to deleted nodes. Either index array may be empty or omitted.
Protocol 2 supports `editNode` and `removeNodes`. Protocol 1 supports bounded
placement and the original `moveNode`, `addNode` and `addLink` edits. The extension applies document changes through `documents/applyEdits` with
that revision as `expectedVersion`. CED requests a new graph after the buffer
revision changes.

## Native tab contributions

The host advertises `host.tabProtocol: 1` in `ced/initialize`. A manifest can declare up to four tabs under `contributes.tabs`. Each has an extension-owned `id`, a `title` of up to 80 UTF-8 bytes, a `panel` of `"sidebar"` or `"terminal"`, and a `command` declared by the same extension. Each tab uses a different command with its normal `onCommand` activation and conditions.

Selecting a tab lazily activates its command in a trusted workspace. The host sends ordinary `commands/invoke` requests with a single element in `arguments`, such as `[{"tabId":"example.tools.panel","action":"open","value":""}]`. Button clicks supply the action ID and an empty value. Submitting the optional input supplies its action ID and up to 8192 UTF-8 bytes of user input. Running the command directly still passes `null` arguments, and a successful result selects the contributed tab. Tab events receive an empty document context.

The command returns a complete model for its tab.

```json
{
  "tab": {
    "heading": "Tools",
    "text": "Ready.",
    "actions": [{"id": "back", "label": "Back"}],
    "input": {"id": "send", "label": "Send", "placeholder": "Message"},
    "refresh": false
  }
}
```

The host renders plain text and native controls. `heading` is limited to 256 bytes, `text` to 24,000 bytes and `actions` to 32 entries with distinct IDs. Action and input IDs are limited to 128 bytes. `input` and `refresh` are optional. If `refresh` is true, the host sends the reserved `refresh` action about every 750 ms while the tab is displayed and idle. It should return a snapshot without side effects. Models are expected to fit the existing 64 KiB command response limit. Models and input drafts are cleared when the extension stops. Hosts that announce `host.workspaceChanges: true` can keep a workspace-independent tab active between trusted workspaces. A tab extension without document capabilities can return `retainOnWorkspaceChange: true` from initialization to opt in. The host verifies the new workspace’s trust and the installed package, then sends `ced/workspaceChanged` with the new `context` and `workspace: {uri, trusted}`. Return `{}` and use that context for subsequent requests. Existing tab models and drafts are retained across the transition.

### Native tab widgets

When `host.tabWidgets` is at least `1`, a model can also set `selectors` and `chat`. The basic tab fields still apply.

- `selectors` holds up to two dropdowns. Each has an `id`, a `label` of up to 80 bytes, a `selected` option ID, an `options` array, and an optional `disabled` flag. Options use `{id, label}` with up to 256 distinct entries and 128 bytes per ID or label. Use an empty `selected` value when there is no selection and an empty `options` array when the control is unavailable. A choice sends the selector ID as `action` and the option ID as `value`. Return the confirmed selection in the next model.
- `chat` has an `author` label of up to 80 bytes and a `messages` array of `{role, text}` objects in chronological order. Roles are `"user"` and `"agent"`. The array supports up to 64 messages and 9,000 bytes of combined message text. Agent messages render as Markdown. When `chat.status` is `"pending"`, `"running"`, or `"failed"`, the tab's `text` displays the corresponding status. The optional `input` becomes a multiline composer and supports an optional `disabled` flag. Enter submits an enabled draft, and Shift+Enter inserts a newline.
- With `host.tabWidgets` at least `2`, a selector can set `placement: "composer"` to appear in the chat composer. Version `3` supports placing both selectors there. Composer selectors use the CUI Model Picker with search for large catalogs. When there is no chat input, the selectors appear above the transcript. A disabled input keeps the composer visible and preserves the draft.
- With `host.tabWidgets` at least `4`, `chat.messages` also accepts `{role: "reasoning", id, text, status?}` and `{role: "tool", id, text, status, input?, output?}` in transcript order. Activity IDs must be unique in the model and stable across refreshes. Reasoning text renders as Markdown and can set `status: "running"` while streaming. Tool rows use `text` as the title and a status of `"pending"`, `"running"`, `"done"`, or `"failed"`. Tool titles are limited to 256 bytes. Optional input and output previews are limited to 1,024 bytes each. Previews count toward the combined transcript text budget.
- With `host.tabWidgets` at least `5`, a chat's `input.stop` can provide an action ID for the composer. The composer shows Stop while this action is available and dispatches it with an empty value. A draft can be edited while Stop is shown and submitted after Send returns. The transcript follows new messages while scrolled to the bottom and offers a jump button while reading earlier messages.
- With `host.tabWidgets` at least `6`, agent messages in the current turn may include an `id` for the typewriter animation. Keep the ID stable as text streams and unique in the transcript. History can omit the ID to display immediately.
- With `host.tabWidgets` at least `7`, `chat.back` can provide an action ID for a left chevron before the chat title. Clicking it dispatches the action with an empty value.
- With `host.tabWidgets` at least `8`, a tab without `chat` can provide `groups: [{label, create, items: [{id, label, delete}]}]`. Each group shows its label, square rows with a delete button each, and a New Session button at the end. Clicking a row dispatches its `id`, and the buttons dispatch the `delete` and `create` action IDs, all with empty values. A model supports at most 32 groups and 32 items total. Group labels are limited to 1,024 bytes and item labels to 256 bytes.
- With `host.tabWidgets` at least `9`, a model can include up to three selectors. Composer selectors appear from left to right in model order. A chat can provide `approval: {title, actions: [{id, label}]}` to present a request above the composer. The title is limited to 320 bytes and the request can contain 1 to 13 actions with labels up to 128 bytes. Clicking an action dispatches its ID with an empty value.
- With `host.tabWidgets` at least `10`, a group item can include `busy: true` to show a native CUI spinner next to its title. The row still accepts clicks. Use the model’s `refresh` flag to keep the state current.

## JVM language-server contributions

An executable package may declare up to two distinct language IDs in `contributes.languageServers`. The implemented IDs are `java` (`.java`) and `kotlin` (`.kt` and `.kts`). For example, `"languageServers": ["java", "kotlin"]` enables the package to provide both. CED chooses the enabled provider by extension ID within the trusted workspace, verifies its installed package on a worker, and holds a shared package lease while the server runs.

For a selected JVM source file, CED starts a separate language-server process from the reviewed platform entrypoint with arguments `--language-server <language-id> <absolute-project-root> [<gradle-jdk> [<server-executable>]]`. Standard input and output in this mode carry LSP. The ordinary no-argument extension command process still uses the CED API and its existing reduced environment. The server launcher receives user tool paths and standard home, locale and build-cache settings. Project settings can select build roots only within the trusted workspace. The optional JDK argument is used for Kotlin Gradle imports. If only the server executable is configured, CED passes an empty string in the JDK position. For JDT LS, the Gradle JDK is sent in the server settings.

CED detects Maven `pom.xml` and Gradle `settings.gradle[.kts]` or `build.gradle[.kts]` files. Workspace `.ced/settings.json` may set `maven.pomPath` or `gradle.projectPath`, and an absolute `gradle.javaHome` for the Gradle wrapper, with fallback to the same keys in `~/.ced/settings.json`. Maven projects also accept `maven.settingsPath`, which selects a user settings XML file for JDT LS and for Kotlin LSP with Maven 3.9 or newer. On Windows, Kotlin Maven settings paths containing whitespace require a short path supplied by the volume. The value can be workspace-relative, absolute or start with `~/`. Use `java.jdtlsPath` or `kotlin.lspPath` to choose the executable by absolute path or a path starting with `~/`. A trusted workspace can override either user setting, and an empty string restores executable discovery. Opening a saved source file starts the detected project. **Import JVM Project** recomputes and reloads it. The status bar reports the server and build type. Syntax coloring is available directly in CED.

## Installation and trust

Use the command palette's **Install Extension from Folder** and **Manage Extensions** actions. Review installation is explicit. Installation, hashing, profile writes, package validation and process I/O run on workers. Filesystem packages are copied without installation scripts; their manifests are parsed from the exact bytes included in the reviewed digest. A SHA-256 digest identifies contents; it does not authenticate a publisher.

The profile is `~/.ced`. Versioned packages live at `extensions/<id>/<version>/`, active versions/grants/enabled state and canonical trusted workspace paths at `extensions.json`, and advisory locks at `extension-locks/`. Profile updates use a temporary file, fsync and atomic rename under a nonblocking cross-window lock. A shared version lease prevents uninstalling a package used by another window. Old packages are retained on update; uninstall acquires leases for all versions (up to 128) before removing the registration and packages. Reinstall a retained package folder to roll back, with a fresh review.

Packages reject links, traversal, special files, unavailable entrypoints, unknown capabilities/contributions, duplicate JSON keys and incompatible identities/API requirements. A registered version cannot be replaced with different contents. Installed contents are revalidated before starting a process. A damaged catalog fails closed; the original profile is preserved for diagnosis.

Workspace trust is never read from repository configuration. Saved documents inside the canonical trusted workspace are authorized by default according to granted capabilities. The manager can grant an extension access to the active untitled/outside document for the current session. Document grants remain tied to that document ID and disappear on manager reload/workspace change; revoking one does not revoke the default workspace access policy. Revoking an explicit document grant stops the affected instance and cancels its pending invocation. Save As/rename also stop an instance when the document loses its authorized scope; closing a document cancels its pending command and removes its explicit grants. Subsequent activation is lazy and starts a fresh process session. Disabling extensions and revoking trust stop existing processes. A failed process requires explicit restart; no command or edit is replayed.

Executables run with the user's operating-system permissions. CED starts the reviewed absolute entrypoint with the installed package as its working directory. On macOS and Linux the command process receives `HOME`, `TMPDIR`, `LANG`, `LC_ALL` and `LC_CTYPE` when present. On Windows it receives `HOME`, `USERPROFILE`, `SYSTEMROOT`, `WINDIR`, `TEMP`, `TMP`, `LANG`, `LC_ALL` and `LC_CTYPE` when present. Command entrypoints should locate extra tools by absolute path. Windows entrypoints must be native `.exe` files, and their stdin and stdout must use binary mode for framing. Runtime requirements are displayed for manual setup. Workspace identity comes from initialization.

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
| Process stop | Deactivate immediately, POSIX TERM after 500 ms, force termination after 1 s |
| User message / rate | 512 bytes / 5 per 10 seconds per instance |
| Log message / rate / rotation | 8 KiB / 64 KiB per second / three 1 MiB files |

Framing accepts one ASCII `Content-Length: <positive-decimal>\r\n\r\n` header and UTF-8 JSON, without a BOM or NUL. Batched messages, numeric/null request IDs, duplicate keys and numbers that overflow to nonfinite values are rejected. Unknown optional RPC fields are ignored; known required fields and mutually exclusive result/error shapes remain validated. Manifest unknown-field validation stays strict. Extension request IDs use increasing `ext:<counter>` values; host IDs use `ced:<counter>`. Responses retain the request ID. Stale document errors include `data.currentVersion`; malformed document IDs/parameters return `InvalidParams` (`-32602`), while closed/unknown well-formed IDs return `Unavailable`. Partial/coalesced reads, partial writes, `EAGAIN`, EOF and broken pipes are handled by the worker. Traffic or allocation overflow terminates the affected instance. Advertised limits can be lower than the draft ceilings; the structural-token ceiling also applies.

Invalid notifications are diagnosed without replies. Diagnostics are rate-limited; eight violations in ten seconds stop the offending instance. The frame budget is cooperative: an individual atomic service can exceed it, but subsequent messages are deferred, and lifecycle deadlines/reaping are still checked for every instance.

The supervisor drains/rate-limits stderr independently and writes logs under `logs/extensions/<session>/<id>.log`, replacing terminal control bytes. Lifecycle request IDs, methods, durations/error codes and confirmed edit revisions are logged without document payloads. Pending records are appended in a bounded buffer rather than replacing each other on every call. The manager displays source, grants, activation time, pending request count and queued bytes. The UI's **Open Log** opens that file as a normal document. Stopping cancels pending work, sends deactivation and ultimately terminates the POSIX process group or Windows job and waits for the direct child. Normal frames do not wait for shutdown. Application shutdown has a bounded process wait; a worker delayed in filesystem I/O detaches with ownership of its own resources. Catalog workers also release UI ownership on shutdown. Interrupted installation can leave an unregistered staging directory, but cannot activate a partially written package/profile.

There is no live cross-window profile watcher yet. Other windows pick up catalog/trust changes when their manager reloads, such as after a workspace switch, a management operation or application restart. Current-window revocation invalidates API access immediately.

## Deferred phases

API 1.0 is experimental until the stable-operation phase is accepted. Persistent extension state, effective configuration/overrides, subscriptions/events, contributed keybindings/menus, status/output/tree views, themes, and declarative diagnostics contributions are deferred. There is no marketplace, automatic update, embedded scripting runtime or public CUI ABI.

Run `make test` for the native integration, failure, protocol, edit and UI suites. `make EXTENSIONS=OFF BUILD_DIR=build-no-extensions test` verifies the editor without the subsystem. Tests do not install anything in the real user profile.
