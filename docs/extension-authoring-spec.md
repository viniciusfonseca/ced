# CED Extension Authoring Specification

**Document revision:** 0.1.0-draft, 2026-11-15.  
**Target:** CED Extension API 1.0, manifest format 1, macOS and Linux.  
**Audience:** coding agents and developers implementing extension packages.  
**Availability:** CED 0.2.0 development implements an experimental subset (architecture phases 0–1). Read the [implemented host profile](extensions.md) before targeting it. Configuration/state/events and later contributions in this specification are not available yet; API 1.0 is not stable.

This specification turns the [extension architecture](arquitetura-extensoes.md) into an author-facing draft contract. Implemented methods use these wire shapes, but the host currently implements only the subset identified in its [profile](extensions.md) and advertised in `hostMethods`. Keep these documents synchronized; an incompatible host must be reported rather than silently worked around. The full draft must be validated before API 1.0 is declared stable.

An agent can build and test an extension against a mock host or the experimental host. It **MUST NOT** claim that an extension runs inside CED unless it has exercised that extension with a compatible host. `ced.minVersion: "0.2.0"` targets the development implementation; it does not announce a release or guarantee every draft feature.

**Navigation:** [agent workflow](#1-agent-implementation-contract) · [manifest](#3-package-and-manifest) · [transport](#5-transport-and-json-rpc-profile) · [types](#6-shared-wire-types) · [lifecycle](#7-lifecycle-and-handshake) · [API](#8-host-service-reference) · [events](#9-events) · [edits](#10-editing-rules) · [example](#11-reference-command-flow) · [tests](#13-conformance-tests) · [delivery](#14-definition-of-done).

## 1. Agent implementation contract

The capitalized terms **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY** express requirements within this draft. MUST requirements are mandatory for conformance. SHOULD requirements may be departed from only with a documented reason and tests covering the alternative. MAY indicates an optional feature.

When asked to build a CED extension, an agent MUST:

1. Inspect the target CED version, this specification, repository instructions, and any available host or SDK. Distinguish implemented features from this draft.
2. Translate the requested behavior into commands, required capabilities, platform targets, configuration, inputs, and observable outputs. Use only defined contribution points and methods.
3. Implement an independent executable or a declarative package. Do not modify CED core unless the user separately requests host work.
4. For executable packages, keep transformations independent of transport code. Implement a framed bidirectional RPC peer, lifecycle handlers, and explicitly registered command handlers.
5. For document features, capture invocation context, read snapshots through the API, and submit version-checked edits. Never replace live document access with direct disk reads or writes.
6. Provide source, a valid manifest, reproducible build instructions, automated tests, and the deliverables in section 14.
7. Report unsupported requested behavior and missing host functionality explicitly. A mock passing is not a successful CED integration test.

If no language is requested, prefer a small standalone C99 executable to match the repository's toolchain. Another language is valid when its runtime and packaging are documented. There is no assumed `ced` language package, SDK import, CLI scaffolder, or extension installation flag.

An agent MUST NOT invent APIs such as `ced.editor.replaceText`, `commands/execute`, `workspace/writeFile`, a VS Code compatibility layer, or a global CED singleton. It MUST NOT bind to `Editor`, `CedDocument`, `workspace.h`, CUI internals, process memory, or the editor's private settings files.

### Supported target and deferred features

| Surface | Draft API 1.0 target | Implementation status in CED 0.1.0 |
| --- | --- | --- |
| Local package, manifest, command activation | Defined here | Not implemented |
| Snapshot reads and single-document edits | Defined here | Not implemented |
| Configuration, state, events, menus, shortcuts | Defined here; architecture phase 2 | Not implemented |
| Status items, output channels, tree views, themes, diagnostics providers | Deferred; no authoring schema yet | Unsupported |
| JVM LSP servers | Experimental host contribution on macOS and Linux | See the [host profile](extensions.md#jvm-language-server-contributions) |
| Formatter contributions, rename, code actions | Deferred | Unsupported |
| Multi-document transactions, webviews, arbitrary UI, save hooks | Outside this version | Unsupported |
| Marketplace, inter-extension dependencies | Outside this version | Unsupported |

Do not put deferred fields into a manifest: unknown fields are rejected. During host development, use the negotiated `hostMethods` list to detect which draft services are actually available.

## 2. Execution and trust model

- CED launches one process per active executable extension per window. The process is bound to that window's current workspace and is activated lazily.
- The executable receives no protocol-specific command-line arguments. Its working directory is the installed package root. It MUST get workspace identity from `ced/initialize`, not from the working directory.
- Standard input and output carry only protocol frames. Standard error is diagnostic output and is drained separately. No banner, debug print, progress indicator, or compiler output may reach stdout.
- CED supplies a reduced environment. The extension MUST NOT require inherited access tokens, shell initialization files, or an ambient working-directory convention. Missing tools produce an actionable error; no automatic dependency installation is allowed.
- Executable extensions require explicit installation/enabling and a trusted workspace. A repository recommendation does not authorize installation or execution.
- Capabilities restrict CED API access. **A native extension process is not sandboxed** and runs with the user's OS privileges. Authors MUST NOT describe capability grants as OS-level filesystem/network confinement.
- A package MUST be treated as read-only at runtime. Store extension state through `state/get` and `state/set`, not beside the executable or in another extension's files.
- Use document API operations for editor content. For a separately requested external tool, use an argument vector and explicit data transfer; never build a shell command from document text, configuration, filenames, or project files.

There is no process, network, terminal, Git, open-file, save-file, or delete-file host API in this version. An extension MUST NOT inject commands into CED's interactive terminal or bypass the document transaction contract by changing an open file on disk.

## 3. Package and manifest

### 3.1 Package layout

The installable artifact is a directory containing `ced-extension.json` at its root:

```text
example.text-tools/
  ced-extension.json
  README.md
  LICENSE
  bin/
    darwin-arm64/text-tools
    darwin-x86_64/text-tools
    linux-x86_64/text-tools
```

Only include supported target binaries. A source repository MAY also contain `src/`, `tests/`, and build files; the distribution directory SHOULD exclude development dependencies and unrelated build output.

Package requirements:

- Maximum 64 MiB of file content, 2,000 files, and a 256 KiB manifest.
- Paths MUST be relative, use `/`, contain no `..` component, and remain inside the package. Symlinks, hard-linked shared payloads, devices, sockets, FIFOs, and install scripts are not allowed.
- Entrypoints MUST be regular executable files. The host launches the validated absolute path without a shell or implicit executable lookup on `PATH`.
- Native binaries MUST match the advertised platform and architecture. A runtime-backed executable needs an executable launcher with a declared, explicitly located interpreter; CED does not resolve or install that interpreter for the author.
- Runtime resources MUST be located relative to the package root, never a source checkout. The package MUST work after being copied to a different installation path.
- An ID/version pair identifies immutable content. Change the package version when changing released bytes; never overwrite the files of a running installed version.

Local folder installation is the proposed installation mechanism. Do not document nonexistent commands such as `ced --install-extension` as working instructions.

### 3.2 Example executable manifest

This example intentionally implements **ASCII-only** uppercase conversion. Its configuration controls what happens when the selection contains non-ASCII characters; it does not pretend to implement Unicode case conversion.

```json
{
  "manifestVersion": 1,
  "id": "example.text-tools",
  "name": "Text Tools",
  "version": "0.1.0",
  "description": "Explicit ASCII transformations of the selected text.",
  "license": "MIT",
  "ced": { "minVersion": "0.2.0", "apiMajor": 1 },
  "kind": "executable",
  "entrypoints": {
    "darwin-arm64": "bin/darwin-arm64/text-tools",
    "darwin-x86_64": "bin/darwin-x86_64/text-tools",
    "linux-x86_64": "bin/linux-x86_64/text-tools"
  },
  "activationEvents": ["onCommand:example.text-tools.uppercase"],
  "capabilities": ["documents.read", "documents.edit"],
  "untrustedWorkspaces": false,
  "contributes": {
    "commands": [
      {
        "id": "example.text-tools.uppercase",
        "title": "Uppercase ASCII Selection",
        "category": "Text",
        "enableWhen": { "editorTextFocus": true, "hasSelection": true }
      }
    ],
    "keybindings": [
      {
        "command": "example.text-tools.uppercase",
        "key": "Mod+Alt+U",
        "when": { "editorTextFocus": true, "hasSelection": true }
      }
    ],
    "menus": [
      {
        "location": "editor.context",
        "command": "example.text-tools.uppercase",
        "group": "transform",
        "order": 10
      }
    ],
    "configuration": [
      {
        "key": "example.text-tools.nonAsciiPolicy",
        "type": "string",
        "default": "reject",
        "scope": "user",
        "description": "Reject non-ASCII selections or preserve non-ASCII characters.",
        "enum": ["reject", "preserve"]
      }
    ]
  }
}
```

### 3.3 Root fields

Unknown fields are errors at every manifest object level, except for arbitrary JSON inside the optional root `metadata` object. Duplicate JSON keys MUST be rejected. Manifest JSON nesting MUST NOT exceed 32 container levels, counting the root as level 1.

| Field | Type / requirement | Meaning |
| --- | --- | --- |
| `manifestVersion` | Required; integer `1` | Manifest format. |
| `id` | Required; string, 1–128 ASCII bytes | Exactly two nonempty components separated by one `.`, each using `[a-z0-9-]+`; publisher `ced` is reserved. |
| `name` | Required; nonempty string | Display name. |
| `version` | Required; version string | Three nonnegative decimal integers `MAJOR.MINOR.PATCH`, no leading zero except `0`, no ranges/prerelease/build suffix. Compare numeric components, not text. |
| `description` | Optional; string | User-facing summary. |
| `license` | Optional; string | License identifier; include license text in the package. |
| `ced` | Required; object | Exactly `minVersion` using the version syntax above and `apiMajor: 1`. |
| `kind` | Required; `"executable"` or `"declarative"` | Whether the package starts a process. |
| `entrypoints` | Required for executable; nonempty object | Platform key → relative executable path. Target keys: `darwin-arm64`, `darwin-x86_64`, `linux-arm64`, `linux-x86_64`, `windows-arm64`, `windows-x86_64`, `windows-i686`. Windows entrypoints use native `.exe` files and binary stdin/stdout. |
| `activationEvents` | Required for executable; nonempty string array | Unique `onCommand:<own-command-id>` entries. Other activation events are deferred. |
| `capabilities` | Required for executable; unique string array, possibly empty | Only `documents.read` and `documents.edit` are defined. Edit requires read. |
| `untrustedWorkspaces` | Required for executable; `false` | Executables cannot run in untrusted workspaces in this version. |
| `requirements` | Optional; array | Each item is exactly `{ "name": string, "minVersion": string }`; informational external tool/runtime requirement, not an install instruction. |
| `contributes` | Optional; object | `commands`, `keybindings`, `menus`, `configuration`, [native tabs](extensions.md#native-tab-contributions), and [JVM language servers](extensions.md#jvm-language-server-contributions). See the host profile for supported arrays. Missing arrays mean empty arrays. |
| `metadata` | Optional; object | Non-executable author metadata, still subject to file size/depth limits. |

Display strings MUST be valid UTF-8, contain no NUL or terminal escape sequences, and fit 512 encoded bytes. Entrypoint paths MUST be nonempty and shorter than 4,096 UTF-8 bytes. Local command suffixes and configuration key suffixes use `[a-zA-Z0-9_-]+` components separated by `.`, following `<extension-id>.`; the full ID/key MUST fit 256 ASCII bytes. IDs/keys are case-sensitive.

Declarative packages MUST omit `entrypoints`, `activationEvents`, `capabilities`, and `untrustedWorkspaces`. In this draft they may contribute configuration only. The presence of a theme or a command handler in a declarative manifest is an error, not a request to execute it.

### 3.4 Contribution objects

Each contribution array has at most 100 entries. Duplicate command IDs or configuration keys are errors. Every referenced command MUST be declared by this package, and every executable command MUST have an `onCommand` activation event. No cross-extension or built-in command execution is exposed.

| Object | Required fields | Optional fields |
| --- | --- | --- |
| Command | `id`, `title` nonempty strings | `category` string, `enableWhen` condition object |
| Keybinding | `command`, `key` strings | `when` condition object |
| Menu item | `location`, `command`, `group` strings; `order` nonnegative safe integer | None |
| Configuration | `key`, `type`, `default`, `scope`, `description` | `enum` nonempty distinct values; `minimum`, `maximum` for number only |

Configuration `type` is `"string"`, `"boolean"`, or `"number"`; `scope` is `"user"` or `"workspace"`. Defaults and enumeration members MUST match the declared type, be finite where numeric, and satisfy constraints. `minimum <= maximum` when both exist. String configuration values fit 512 UTF-8 bytes. No field may configure code entrypoints, API authorization, interpreter paths, or shell fragments.

Conditions are AND-only objects with these keys: `editorTextFocus`, `hasSelection`, `hasDocument`, `workspaceTrusted` (boolean), and `languageId` (string). An omitted/empty condition is true. These are declarative checks, not scripts; strings containing expressions are invalid. Handlers MUST revalidate required input even when a condition enabled the command.

Allowed menu locations are `editor.context` and `explorer.context`. No icon field is defined in draft 0.1.0. Shortcut syntax is one combination: modifiers in canonical `Mod+Ctrl+Alt+Shift` order, followed by one uppercase letter, digit, or `F1`–`F12`. Include only modifiers used; duplicates and combining `Mod` with `Ctrl` are invalid. `Mod` means Command on macOS and Ctrl on Linux. Chords and unmodified printable keys are not supported. User mappings win over built-ins, which win over extension defaults; extension conflicts are ordered by extension ID. The handler MUST NOT bypass shortcut focus rules.

The palette and menus evaluate editor conditions against the originating editor context captured before opening the overlay. Their temporary focus does not invalidate an otherwise valid text command. Focus in the terminal or an unrelated modal is not editor text focus.

## 4. Capabilities and document access

`documents.read` permits authorized document metadata, editor context, snapshots, and related events. `documents.edit` additionally permits `documents/applyEdits`; it MUST be requested together with read. Requested capabilities are not automatically granted. Use `grantedCapabilities` from initialization as the authority.

Basic active-instance services—configuration, own state, messages, and logs—do not require a named capability. They never allow access to another extension's namespace.

Default document access covers saved documents inside the trusted canonical workspace root. Untitled buffers and open files outside that root require an explicit per-document grant from the user. An unauthorized document is omitted from lists/events; its path, ID, text, and selection MUST NOT be inferred or requested through another route.

Save As and rename can remove access without closing the physical tab. Discard local cached content/handles on access revocation, invalid context, deactivation, or connection loss. Native code cannot be made to forget data by the host; discarding it is also an author obligation. Never log document content by default.

## 5. Transport and JSON-RPC profile

The envelope uses [JSON-RPC 2.0](https://www.jsonrpc.org/specification). CED's profile restricts the accepted shapes and defines its own framing, methods, and domain errors. No network connection is needed.

### 5.1 Framing

Write one ASCII header and an exact-length UTF-8 body:

```text
Content-Length: <positive decimal byte count>\r\n
\r\n
<JSON object bytes>
```

The `\r\n` notation above denotes actual CR/LF bytes. It is not literal backslash text on the wire. Do not append a delimiter after the body. The next frame starts immediately after the declared number of bytes.

A correct encoder performs these operations in order:

```text
body = UTF8_ENCODE(JSON_SERIALIZE(message))
header = ASCII_ENCODE("Content-Length: " + DECIMAL(LENGTH(body)) + "\r\n\r\n")
WRITE_ALL_SERIALIZED(header, body)
FLUSH_STDOUT()
```

`LENGTH(body)` is a byte count after JSON escaping and UTF-8 encoding, never a character count. One writer/queue MUST serialize complete frames; workers MUST NOT interleave output. Flush each completed frame. The reader MUST handle fragmented headers/bodies, several frames in one read, short writes, `EINTR`, and nonblocking backpressure if nonblocking I/O is used.

The header has one case-sensitive `Content-Length` field and no other fields in this draft. Accept decimal digits with a positive value, reject signs/overflow, duplicate fields, and malformed separators. Reject headers over 1,024 bytes or bodies over the negotiated limit before allocating that amount. Framing errors close the connection; there is no newline-based recovery. EOF discards any partial frame and terminates the instance. Broken stdout MUST NOT leave a process mutating documents or waiting indefinitely.

### 5.2 Envelope and dispatch

| Message | Exact required shape |
| --- | --- |
| Request | `{ "jsonrpc": "2.0", "id": string, "method": string, "params": object }` |
| Notification | `{ "jsonrpc": "2.0", "method": string, "params": object }` |
| Success | `{ "jsonrpc": "2.0", "id": string, "result": JSONValue }` |
| Failure | `{ "jsonrpc": "2.0", "id": string-or-null, "error": { "code": integer, "message": string, "data"?: JSONValue } }` |

- Host-originated requests use `ced:<positive-decimal-counter>`; extension requests use `ext:<positive-decimal-counter>`. Start counters at 1 and never reuse an ID on the connection. Responses echo the request ID unchanged.
- Batch arrays, positional parameters, numeric/null request IDs, duplicate object keys, nonfinite numbers, UTF-8 errors, BOMs, and unpaired Unicode surrogates are invalid in this profile. JSON nesting is limited by `maxJsonDepth`, counting the root container as level 1.
- Responses contain exactly one of `result` and `error`. Unknown optional response/message fields are ignored for forward compatibility; known fields are validated. Do not relax the manifest's stricter unknown-field rule.
- Every request receives exactly one terminal response while the connection remains usable. Notifications never receive responses. A successful void operation returns `{}` rather than omitting `result`.
- Except for `ced/initialize`, every request/notification includes `params.context`. Results are matched to the context stored with the pending request; they do not repeat it.
- A response to an unknown/expired ID is discarded and diagnosed without creating a new request. Duplicate live request IDs are protocol violations; never execute the same request twice.
- Unknown request methods return `-32601`. Malformed params return `-32602`; malformed RPC objects, `-32600`; malformed JSON, `-32700`, using `id: null` when no valid ID can be recovered. Notification errors are logged locally without a response.
- A known service absent from `hostMethods` returns `UnsupportedCapability`. A supported service without a grant returns `PermissionDenied`. Private or deferred methods MUST NOT be guessed.

The peer MUST continue reading and dispatching while awaiting a response. In particular, `commands/invoke` can stay pending while its handler sends `documents/get` and `documents/applyEdits`. A loop that waits synchronously for a command to finish before reading stdin will deadlock.

## 6. Shared wire types

The notation below describes JSON, not a TypeScript runtime or an SDK. `?` means a property may be omitted. Unless explicitly nullable, a missing optional property MUST be omitted rather than sent as `null`. All unmarked properties are required.

```typescript
type JSONValue = null | boolean | number | string | JSONValue[] |
                 { [key: string]: JSONValue };
type SafeUInt = number; // integer, 0 <= value <= 9007199254740991
type PositiveInt = number; // SafeUInt >= 1
type Version = string; // MAJOR.MINOR.PATCH, as defined for the manifest
type Capability = "documents.read" | "documents.edit";

type Context = {
  sessionId: string;
  workspaceId: string;
  generation: SafeUInt;
};
type Selection = { anchor: SafeUInt; cursor: SafeUInt };
type DocumentInfo = {
  documentId: string;
  uri: string | null;
  languageId: string;
  version: PositiveInt;
  dirty: boolean;
  textAccessible: boolean;
  eol: "lf" | "crlf";
};
type EditorContext = {
  document: DocumentInfo | null;
  selection: Selection | null;
  selectionVersion: SafeUInt;
};
type TextEdit = { start: SafeUInt; end: SafeUInt; text: string };
type Scope = "global" | "workspace";
type MessageLevel = "info" | "warning" | "error";
```

Context IDs and document IDs are nonempty opaque strings of at most 128 UTF-8 bytes. Do not parse them as paths, compare their numeric suffixes, generate them yourself, or persist them across sessions. File URIs are absolute host-provided strings; untitled documents have `uri: null`. A URI never grants API access or overrides an ID. `languageId` is an opaque host string of at most 128 UTF-8 bytes.

`version` identifies text only. Metadata can change without incrementing text version. `dirty` reflects comparison with saved contents, not whether the extension has ever edited it. `eol` describes disk serialization; document text is exposed with LF newlines. A host may mark a legacy non-UTF-8 buffer `textAccessible: false`; attempts to read/edit its text return `Unavailable` without silently converting it.

For `EditorContext`, `document: null` requires `selection: null`; an authorized text-accessible document has a selection with byte offsets. A document with inaccessible text may have `selection: null`. When the active document is unauthorized, return `document: null` and `selection: null`, exactly as for no active document. This deliberately avoids disclosing an unauthorized target.

## 7. Lifecycle and handshake

The extension transitions through `waiting-for-initialize → initialized → active → stopping → exited`. Requests in the wrong state receive `NotActive`, except that repeated initialization receives `Conflict`. During the first two states, the extension MUST NOT call host services. Register handlers locally during activation; do not request documents or configuration before returning its activation response.

### 7.1 `ced/initialize` — host request

```typescript
type InitializeParams = {
  host: { name: "CED"; version: Version; tabProtocol?: 1; tabWidgets?: 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10; workspaceChanges?: true };
  extension: { id: string; version: Version };
  apiVersions: string[]; // exact values, initially ["1.0"]
  context: Context; // establishes context; no prior-context check
  workspace: { uri: string; trusted: boolean };
  grantedCapabilities: Capability[];
  hostMethods: string[]; // extension -> host services actually implemented
  limits: Limits;
};
type InitializeResult = {
  extension: { id: string; version: Version };
  apiVersion: string;
  requiredHostMethods: string[];
  retainOnWorkspaceChange?: true;
};
```

The extension MUST verify its expected ID/version, choose an offered API version it implements, and check required grants and services. Echo its compiled/package identity, not an arbitrary identity supplied by an untrusted project. `requiredHostMethods` lists the host methods necessary for its advertised behavior. The host verifies that list before activation. Optional features may use other advertised services after activation.

`hostMethods` contains unique names from section 8 plus `events/subscribe` and `events/unsubscribe`. It does not list lifecycle methods, `commands/invoke`, or host-originated `events/publish`; those follow the negotiated API. `ced/cancel` is part of the lifecycle protocol in both directions and does not require a service-list entry. `requiredHostMethods` is a unique subset of `hostMethods`.

No common version or missing required service returns `UnsupportedCapability`; missing required grants returns `PermissionDenied`; incompatible identity returns `Conflict`. The host does not activate a failing instance. Version `1.0` by itself is insufficient during draft host development: inspect `hostMethods` too.

All `Limits` fields below are required positive integers. Defaults are draft values; use the handshake values and fail clearly if an essential operation cannot fit. A host may lower quotas; it must not advertise values exceeding the draft safety ceilings for byte/count limits without revising the contract. Time budgets are host-supplied operational values, not promises about scheduling precision.

| `Limits` property | Default | Meaning |
| --- | --- | --- |
| `maxHeaderBytes` | 1024 | Includes final header separator. |
| `maxMessageBytes` | 16777216 | Serialized body bytes in either direction. |
| `maxJsonDepth` | 64 | Maximum JSON container nesting. |
| `maxQueuedBytes` | 16777216 | Maximum queued serialized bytes per direction per instance. |
| `maxPendingRequests` | 64 | Live outbound requests per peer. |
| `maxDocumentBytes` | 2097152 | UTF-8 document text bytes, excluding the C terminator. |
| `maxEditsPerTransaction` | 1000 | Edit array count. |
| `maxSubscriptions` | 32 | Active subscriptions per instance. |
| `maxCommandArgumentsBytes` | 65536 | Serialized `arguments` value. |
| `maxCommandResultBytes` | 65536 | Serialized command `result` value. |
| `maxStateBytes` | 262144 | Serialized state value per scope. |
| `maxPresentationBytes` | 512 | User-facing message/label bytes. |
| `maxLogMessageBytes` | 8192 | One log message's UTF-8 bytes. |
| `maxLogBytesPerSecond` | 65536 | Per-instance log retention rate. |
| `maxMessagesPer10Seconds` | 5 | User-facing message rate, separate from RPC traffic. |
| `initializeTimeoutMs` | 3000 | Initialization response budget. |
| `activateTimeoutMs` | 2000 | Activation response budget. |
| `commandTimeoutMs` | 5000 | Command response budget. |
| `serviceTimeoutMs` | 1000 | Ordinary broker request budget. |
| `deactivateTimeoutMs` | 500 | Graceful deactivation budget. |

Implement `Limits` as an object with exactly these named known fields plus ignored future optional fields, not an array or positional tuple. Before receiving initialization, use the fixed header/body/depth ceilings above. The response to initialization itself must fit the newly advertised limits.

### 7.2 `ced/activate` — host request

Params: `{ context: Context, reason: { event: string } }`. In this version the event is exactly a declared `onCommand:<id>`. Result: `{}`.

The host sends this once after successful initialization. Set up command dispatch and return promptly. After its success response has been flushed, the extension may call host services and accept commands. If activation fails, return an error and await host shutdown; do not partially operate.

### 7.3 `commands/invoke` — host request

```typescript
type InvokeParams = {
  context: Context;
  commandId: string;
  arguments: JSONValue; // null when no arguments are supplied
  editor: EditorContext; // captured at user invocation, before lazy activation
  target?: { uri: string; kind: "file" | "directory" }; // explorer menu only
};
```

The result is an extension-defined JSON value within `maxCommandResultBytes`; `{}` is valid. Document success/error/result shapes for every command in the extension README. Validate `arguments` as untrusted input. A target URI is context only, not a disk-read grant.

Only one invocation may run at a time per extension in this version; a concurrent invocation receives `Busy`. Unknown command IDs receive `-32601`; malformed command arguments receive `-32602`. A declared handler MUST NOT switch to a different active document when asynchronous work finishes. Use the captured editor context unless the command's documented behavior explicitly obtains a new target.

If a command times out or is cancelled, all descendant requests/background jobs created for that invocation MUST be tracked and cancelled or awaited. Stop issuing new edits. A host-confirmed edit cannot be undone merely because the parent command later times out. Do not return a failure that claims no effect if a commit has already succeeded.

A long-running job MAY outlive a successful command response only if this behavior is explicit in the command's documentation. It remains owned by the active instance, stops on deactivation, re-reads current snapshots before editing, and never treats a past invocation as an indefinite write authorization.

### 7.4 `ced/deactivate` — host request

Params: `{ context: Context, reason: "disable" | "uninstall" | "workspaceChanged" | "shutdown" | "restart" }`. Result: `{}`.

Stop accepting new work, cancel jobs and pending requests, release local resources, respond within the deadline, then exit. Do not call host services, save documents, prompt the user, or wait for a state write during deactivation. State must have been persisted during normal operation. The host may close the channel without this request after failure.

The host can terminate the process group after the grace period. Child processes MUST NOT detach or outlive the extension. The extension MUST also stop on stdin EOF, broken output, `SIGTERM`, or an invalid session; no automatic reconnection or command replay is defined.

### 7.5 `ced/cancel` — either direction, notification

Params: `{ context: Context, requestId: string }`. No result. The sender cancels its own outstanding request; the receiver correlates the ID in that request's direction. Unknown/already-completed IDs are a no-op.

Cancellation is cooperative and races with success. Before a mutation commits, cancellation can yield `Cancelled`; after commit, success remains valid. Never roll back user-visible changes by issuing an inverse edit automatically. A timeout or channel loss during an edit is an **unknown outcome** unless a terminal response was received; do not retry a mutation blindly.

## 8. Host service reference

All methods in this section are extension → host requests except `log/write`, a notification. Every parameter object includes `context`. Services are usable only while active and when advertised in `hostMethods`. Do not assume that the host implements methods simply because this document lists them.

### 8.1 `editor/getContext`

Params: `{ context: Context }`. Result: `EditorContext`. Requires `documents.read`.

Returns the current authorized editor context. It does not replace the frozen `editor` supplied with an earlier invocation. `selectionVersion` changes when the originating editor, document, or selection changes, including moving away and back. Opening the palette/menu over the same editor does not itself change that token.

### 8.2 `documents/list`

Params: `{ context: Context }`. Result: `{ documents: DocumentInfo[] }`. Requires `documents.read`.

Returns only authorized open documents, without text, up to the host's current tab limit. Ordering is unspecified. Closing and reopening a path creates a new ID; reordering tabs does not. Never locate the target by array index.

### 8.3 `documents/get`

Params: `{ context: Context, documentId: string }`. Result: `{ document: DocumentInfo, text: string }`. Requires `documents.read`.

Text and metadata are captured consistently. Text is valid UTF-8 with no NUL, uses LF newlines, and includes unsaved changes. `textAccessible: false` produces `Unavailable`. An unknown/closed ID produces `Unavailable`; an existing unauthorized ID produces `PermissionDenied` without disclosing metadata. A snapshot may already be stale when received: it is not a lock.

### 8.4 `documents/applyEdits`

```typescript
type ApplyEditsParams = {
  context: Context;
  documentId: string;
  expectedVersion: PositiveInt;
  expectedSelectionVersion?: SafeUInt;
  label: string;
  edits: TextEdit[];
  selection?: Selection; // positions in the final text
};
type ApplyEditsResult = {
  documentId: string;
  version: PositiveInt;
  changed: boolean;
};
```

Requires both document capabilities. One request edits one open document atomically, creates at most one undo step, and never saves. All validation precedes changes. The host revalidates context, document access, and expected versions immediately before commit. Section 10 defines exact range and selection semantics.

`label` is nonempty display text within `maxPresentationBytes`. A changed transaction increments text version once. A no-op returns `changed: false` and the existing version, without undo or a text-change event. An empty edit list with a selection override is rejected as invalid params; this method is not a general selection-setting API.

### 8.5 `configuration/get`

Params: `{ context: Context, keys?: string[] }`. Result: `{ revision: PositiveInt, values: { [fullKey: string]: string | boolean | number } }`. Basic service.

Omit `keys` to read all own declared keys; an empty list requests none. Unknown or foreign keys are invalid params. Values are effective validated values, using `manifest default < user setting < workspace setting`. Only keys declared with `scope: "workspace"` may use workspace overrides. Invalid settings fall back to the last valid value or default and are diagnosed by the host.

There is no configuration write API. Revisions belong to the extension's effective configuration for this context. Use changes as cache invalidations; re-read rather than relying on an old value. Project values are data, not code or permission grants.

### 8.6 `state/get` and `state/set`

```typescript
type StateGetParams = { context: Context; scope: Scope };
type StateResult = { revision: SafeUInt; value: JSONValue };
type StateSetParams = {
  context: Context;
  scope: Scope;
  expectedRevision: SafeUInt;
  value: JSONValue;
};
type StateSetResult = { revision: PositiveInt };
```

These are basic services for the calling extension's namespace only. Global scope is shared across its windows; workspace scope is tied to the canonical project. An absent state returns `{ "revision": 0, "value": null }`. Set atomically replaces the entire JSON value within `maxStateBytes` and increments revision, including when storing the same value. Cross-window races return `Conflict`; read again and merge only if the extension's semantics allow it. A timed-out set has an unknown outcome and requires a read before deciding what to do next.

State is persistent application data, not a secret store, source of authorization, or place for document contents. Do not store tokens, transient Context/Document IDs, pending request IDs, or captured selections. No state-change subscription or deletion API is defined; storing `null` is permitted but still creates a revision.

### 8.7 `window/showMessage`

Params: `{ context: Context, level: "info" | "warning" | "error", message: string }`. Result: `{}`. Basic service.

Messages are nonempty plain text within `maxPresentationBytes`, with no NUL/terminal escapes, HTML, action callbacks, or automatic link execution. The host attributes them to the extension. Respect `maxMessagesPer10Seconds`; excess may be grouped or suppressed even when the request succeeds. Returning an RPC error does not justify showing repeated identical messages as well.

### 8.8 `log/write`

Params: `{ context: Context, level: "debug" | "info" | "warning" | "error", message: string }`. Notification, no response. Basic service.

Respect log byte limits. Log method, duration, operation name, and error reason rather than document text, full arguments, paths, credentials, or environment values. Plain stderr diagnostics obey the same content policy and MUST remain bounded. Never send logs through stdout outside framed messages.

## 9. Events

### 9.1 Subscribe and unsubscribe

`events/subscribe` params: `{ context: Context, events: EventName[] }`. Result: `{ subscriptionId: string, sequence: 0 }`. The event list is nonempty, unique, and contains only supported names. Each subscription belongs to this instance; it counts against `maxSubscriptions`. Document and editor events require `documents.read`; `configuration.changed` is basic and restricted to the caller's keys.

`events/unsubscribe` params: `{ context: Context, subscriptionId: string }`. Result: `{}`. Repeating removal of one's own subscription is a no-op. A foreign subscription is not accessible. Already queued notifications can arrive after local cancellation: discard them after unsubscribing.

The subscribe response is sent before its first notification. Subscribe before reading initial state, then obtain `documents/list` and/or `configuration/get`. Treat notifications as invalidations; a snapshot response and buffered metadata events need not describe the same instant. Do not overwrite newer queried state with an old buffered payload.

### 9.2 `events/publish` — host notification

```typescript
type EventName = "document.opened" | "document.changed" |
  "document.saved" | "document.closed" | "document.metadataChanged" |
  "editor.contextChanged" | "configuration.changed";

type PublishParams = {
  context: Context;
  subscriptionId: string;
  sequence: PositiveInt;
  name: EventName;
  data: object; // exact variants in the table below
};
```

| Event name | `data` shape |
| --- | --- |
| `document.opened` | `{ document: DocumentInfo }` |
| `document.changed` | `{ documentId: string, version: PositiveInt }` |
| `document.saved` | `{ documentId: string, version: PositiveInt }` |
| `document.closed` | `{ documentId: string, reason: "closed" | "accessRevoked" }` |
| `document.metadataChanged` | `{ document: DocumentInfo }` |
| `editor.contextChanged` | `{ editor: EditorContext }` |
| `configuration.changed` | `{ revision: PositiveInt, keys: string[] }` |

`sequence` starts at 1 and increases by one per delivered notification in that subscription, independent of document versions. It represents delivery order, not every keystroke. Changed/context/metadata events may be coalesced before delivery; open, close, and save preserve their order. A document revision may therefore jump. Do not infer text deltas or call an edit using an event alone; request a snapshot.

On a nonconsecutive received sequence, invalidate caches and resynchronize; repeated/malformed sequence errors are a protocol failure. Essential event overflow closes the instance rather than silently dropping close/save events. Access revocation emits a synthetic closed event without a replacement path. These are observations, never save/close veto hooks. Own edits can generate changed events: avoid feedback loops and do not recursively transform every change without an explicit feature requirement.

## 10. Editing rules

### 10.1 Text and offsets

All edit ranges are **zero-based UTF-8 byte offsets**, with an inclusive start and exclusive end: `[start, end)`. Anchor and cursor use the same units; anchor may be greater than cursor. Selection extent is `[min(anchor,cursor), max(anchor,cursor))`.

For `"aé🙂z"`, UTF-8 codepoint boundaries are `0, 1, 3, 7, 8`. Selecting `é🙂` means `[1,7)`, not two character indexes or four UTF-16 units. Range `[2,7)` splits `é` and is invalid. JavaScript string indexes and Python string indexes are not automatically protocol offsets. Encode/decode explicitly or use an offset map; do not slice a language-native string with byte offsets.

Inserted text MUST be valid Unicode/UTF-8, contain no NUL, use LF line endings, and fit the final document byte limit. Do not insert CRLF or a carriage return as a newline. A legacy standalone CR received in existing text is a character, not an extra protocol line break; preserve it unless the requested transformation explicitly changes it. Byte limits are computed before adding the implementation's C terminator. JSON escaping does not affect document offsets, only frame size.

### 10.2 Transaction validation

A conforming extension MUST:

1. Use an authorized captured document ID. Selection commands MUST include the invocation's `expectedSelectionVersion`.
2. Read a snapshot and compare its version with the captured invocation version before calculating selection-based edits. If different, abort with `StaleDocument`; do not reinterpret an old selection over new text.
3. Compute all offsets against that single snapshot. Require `0 <= start <= end <= byteLength(text)` and codepoint boundaries.
4. Sort the edit list by ascending start. Reject overlapping ranges, duplicate insertions at one position, and an insertion sharing the start or end boundary of another edit. Adjacent nonempty ranges are permitted.
5. Check `maxEditsPerTransaction`, serialized frame size, and final text byte length with overflow-safe arithmetic. Never truncate replacement text to fit.
6. Submit one request with `expectedVersion` equal to the snapshot version. The host applies ranges from end to start, validates the whole transaction, and rejects it without partial effects on failure.
7. Use the success response's version as authoritative. Do not assume disk save, automatic formatting, a particular active tab, or exclusive ownership of the buffer.

Text version increases once per effective text transaction, including undo and redo; it does not increase on a pure save, rename, tab reorder, or no-op edit. Two writers submitting the same expected version cannot both commit: the later one gets `StaleDocument`.

The optional `selection` applies to the final text and must be within its boundaries. Without it, the host maps anchor/cursor through edits: a position inside `[start,end)` moves to the replacement end; one at/after `end` gets the accumulated delta; a position exactly at an insertion moves after the inserted text. Adjacent nonempty ranges are processed in snapshot order, so a position at their shared boundary follows the second range's start rule. Mapping is identical for anchor and cursor regardless of selection direction.

A no-op request MUST NOT move the selection, create undo, increase version, or emit a text-change event. If the caller supplied a different selection with a text no-op, the host ignores that selection. The selection facility is part of a real text transaction, not a cursor command.

### 10.3 Failure handling

| Condition | Required response by the extension |
| --- | --- |
| `StaleDocument` / `StaleEditorContext` | Stop this transformation; tell the caller to invoke again if appropriate. Do not replay original offsets. |
| `PermissionDenied` | Stop; explain the missing grant without probing disk or another API. |
| `InvalidContext`, closed document, deactivation, EOF | Cancel work and discard session-bound handles. |
| `ResourceLimit` | Return a bounded failure or reduce a future read-only workload; never split an atomic edit into partial commits silently. |
| Timeout/disconnect during apply | Mark outcome unknown; do not retry or claim that no edit occurred. |
| Apply succeeded but final command response was lost | Preserve the committed effect; no replay or inverse edit. |

Mutations have no automatic retry, reconnect replay, transaction deduplication token, or distributed exactly-once guarantee in API 1.0. Read-only queries MAY be retried within the same active context when still useful; retrying them does not authorize recomputing a user selection command against a different target.

## 11. Reference command flow

This is a test fixture for `example.text-tools.uppercase`, not a full implementation. A mock host owns document `doc-18`, revision 41, with text `"hello world\n"`, selection `[6,11)`, and selection token 9. Workspace/context strings are opaque fixture values. Real initialization still has to complete first.

### 11.1 Lifecycle example

The initialization request uses `InitializeParams`, including every `Limits` property from section 7.1, both granted document capabilities, and at least these advertised methods: `documents/get`, `documents/applyEdits`, `configuration/get`. The extension replies:

```json
{
  "jsonrpc": "2.0",
  "id": "ced:1",
  "result": {
    "extension": { "id": "example.text-tools", "version": "0.1.0" },
    "apiVersion": "1.0",
    "requiredHostMethods": [
      "documents/get",
      "documents/applyEdits",
      "configuration/get"
    ]
  }
}
```

The host then sends `ced/activate` as `ced:2` and receives `{}`. After activation, the extension queries `configuration/get` as `ext:1`; the mock returns `{ "revision": 1, "values": { "example.text-tools.nonAsciiPolicy": "reject" } }`. The selected ASCII input below satisfies either supported policy.

### 11.2 Invocation and snapshot

```json
{
  "jsonrpc": "2.0",
  "id": "ced:3",
  "method": "commands/invoke",
  "params": {
    "context": {
      "sessionId": "session-7", "workspaceId": "workspace-3", "generation": 2
    },
    "commandId": "example.text-tools.uppercase",
    "arguments": null,
    "editor": {
      "document": {
        "documentId": "doc-18", "uri": "file:///tmp/project/notes.txt",
        "languageId": "text", "version": 41, "dirty": true,
        "textAccessible": true, "eol": "lf"
      },
      "selection": { "anchor": 6, "cursor": 11 },
      "selectionVersion": 9
    }
  }
}
```

While `ced:3` remains pending, the extension sends:

```json
{
  "jsonrpc": "2.0",
  "id": "ext:2",
  "method": "documents/get",
  "params": {
    "context": {
      "sessionId": "session-7", "workspaceId": "workspace-3", "generation": 2
    },
    "documentId": "doc-18"
  }
}
```

The mock replies:

```json
{
  "jsonrpc": "2.0",
  "id": "ext:2",
  "result": {
    "document": {
      "documentId": "doc-18", "uri": "file:///tmp/project/notes.txt",
      "languageId": "text", "version": 41, "dirty": true,
      "textAccessible": true, "eol": "lf"
    },
    "text": "hello world\n"
  }
}
```

The extension verifies revision 41 and slices bytes `[6,11)`, obtaining `"world"`.

### 11.3 Commit and completion

```json
{
  "jsonrpc": "2.0",
  "id": "ext:3",
  "method": "documents/applyEdits",
  "params": {
    "context": {
      "sessionId": "session-7", "workspaceId": "workspace-3", "generation": 2
    },
    "documentId": "doc-18",
    "expectedVersion": 41,
    "expectedSelectionVersion": 9,
    "label": "Uppercase ASCII Selection",
    "edits": [{ "start": 6, "end": 11, "text": "WORLD" }],
    "selection": { "anchor": 6, "cursor": 11 }
  }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": "ext:3",
  "result": { "documentId": "doc-18", "version": 42, "changed": true }
}
```

```json
{
  "jsonrpc": "2.0",
  "id": "ced:3",
  "result": { "changed": true, "version": 42 }
}
```

Expected buffer: `"hello WORLD\n"`; no disk save; one undo restores `"hello world\n"`. The example command documents its result as `{ changed: boolean, version: PositiveInt }`. If input was already uppercase, return `changed: false` with the current version; no edit request is necessary.

For policy `reject`, any non-ASCII selected scalar produces command error `-32602` with a concise explanation and no apply request. For `preserve`, replace only ASCII `a`–`z` with `A`–`Z` and copy every other Unicode scalar unchanged. A production Unicode-uppercase command needs a documented Unicode implementation and tests for length-changing transformations; do not use C `toupper` on individual UTF-8 bytes.

If the mock changes text to revision 42 before apply, answer instead:

```json
{
  "jsonrpc": "2.0",
  "id": "ext:3",
  "error": {
    "code": -32002,
    "message": "Document changed before the edit could be applied.",
    "data": { "reason": "StaleDocument", "currentVersion": 42 }
  }
}
```

The extension forwards a terminal failure for `ced:3` and issues no second apply request. If only selection changed, use `StaleEditorContext` instead. If the connection disappears, do not fabricate a host response or report a confirmed outcome.

## 12. Errors and operational behavior

Error `code` and `data.reason` identify domain errors; `message` is plain human-readable text and MUST NOT be parsed for control flow. Base JSON-RPC errors use their standard codes and do not require a domain reason. Internal errors use `-32603` without leaking stack traces or document data.

| Code | Stable `data.reason` | Optional diagnostic data |
| --- | --- | --- |
| `-32001` | `PermissionDenied` | `capability` if applicable |
| `-32002` | `StaleDocument` | `currentVersion` |
| `-32003` | `InvalidContext` | None; do not disclose a replacement context |
| `-32004` | `Cancelled` | None |
| `-32005` | `ResourceLimit` | `limit` naming a known quota |
| `-32006` | `Busy` | None |
| `-32007` | `NotActive` | None |
| `-32008` | `UnsupportedCapability` | `method` if applicable |
| `-32009` | `Timeout` | None |
| `-32010` | `Conflict` | `currentRevision` for own state if available |
| `-32011` | `Unavailable` | None |
| `-32012` | `StaleEditorContext` | None |

Receivers MUST handle all listed reasons and fail safely on an unknown error without retrying a mutation. Domain failures from command handlers SHOULD use these codes when applicable. Extension-specific user input errors use `-32602`; the command README explains valid inputs. Do not allocate new domain codes informally inside this reserved profile.

Limits are backpressure, not a throughput target. The proposed host initially permits four active executable extensions per window, enforces aggregate transport/parsed-data budgets, and may deny a new instance rather than evict one. Authors MUST bound queues, worker counts, logs, cache size, and child-process lifetime themselves. A process boundary does not limit the extension's own CPU or RAM.

Avoid polling snapshots or configuration continuously; subscribe to invalidation events when available. Never ask for every open buffer per keystroke. Persist useful state during active operation and handle cross-window revision conflicts. Preserve ordinary file editing when the extension is disabled or fails.

## 13. Conformance tests

Tests MUST run independently of the editor's graphical window and external services. For an executable package, launch the actual packaged executable through pipes in a mock host. The mock implements the exact draft envelopes, lifecycle, capability filtering, document revisions, and edit responses used by the extension. A mock test cannot certify real host behavior or real undo history; label those checks separately. Declarative packages run manifest/package/configuration tests only; transport, process, and document cases do not apply to them.

### 13.1 Required automated cases

| ID | Case | Expected evidence |
| --- | --- | --- |
| M01 | Parse own manifest and inspect staged package. | Required fields, IDs, command references, target files, executable bits, and size limits pass; bad duplicates/paths/unknown fields fail. |
| P01 | Feed frames one byte at a time and several at once. | Same decoded messages; UTF-8 byte length correct; no stdout contamination. |
| P02 | Exercise non-ASCII JSON, oversized header/body, invalid length, duplicate keys, invalid JSON/UTF-8, and excessive nesting. | Bounded rejection before untrusted large allocation; no crash or desynchronization. |
| P03 | Send invoke while its handler needs host responses; deliver unrelated notifications and out-of-order responses. | No deadlock; IDs correlate; only one terminal response. |
| L01 | Initialize/activate normally; attempt service work too early and repeat initialization. | No service request before activation; wrong states fail deterministically. |
| L02 | Missing grant/service, wrong package identity, unsupported API version. | No activation or fallback to private/disk APIs. |
| L03 | Deactivate/EOF/SIGTERM while a command is working. | Jobs and children terminate promptly; no new edits or blocking shutdown. |
| C01 | Valid command, unknown command, invalid arguments, empty selection, concurrent invocation. | Defined result or bounded error; `Busy` for concurrency; no unintended mutation. |
| D01 | Transform an unsaved snapshot different from disk. | Output uses host snapshot only. |
| D02 | Change document version, move selection away/back, close/reopen same path, switch context. | No stale apply or second target substitution; correct failure. |
| D03 | Apply conflicts after calculation, cancel during apply, disconnect after simulated commit. | No blind retry, duplicate commit, or claim of confirmed failure after known success. |
| U01 | Unicode boundaries in `aé🙂z`, reversed selection, and variable-length replacement. | Byte-correct slicing and final selection; invalid boundaries rejected. |
| U02 | Two valid ranges, overlap, duplicate insertion, ambiguous boundary insertion, no-op, growth beyond limit. | One apply request for valid batch; no partial/truncated edits for invalid input. |
| E01 | Subscribe, combine changed events, revoke access, unsubscribe with queued events. | Cache invalidation and cleanup; no event-triggered edit loop. Required when using events. |
| S01 | Configuration defaults/override validation and state compare-and-set conflict. | Own namespace only; no lost update or secret persistence. Required when using those services. |
| R01 | Slow host and saturated local output/worker queues. | Memory is bounded, cancellation still dispatches, no stdout interleaving. |
| K01 | Copy staged package to another directory and launch each available platform artifact. | No source-tree path assumptions; requirements documented for unavailable targets. |

Pure transformations SHOULD also be unit-tested separately from RPC. A `read()` implementation that only accepts a whole frame at once, a JSON fixture that never launches the executable, or a test that only checks replacement text is insufficient coverage for an executable extension.

### 13.2 Real-host integration checks

Once a compatible host exists, record its commit/version and test: local installation; lazy activation through the palette; capability denial; editing an unsaved buffer; one undo and redo; a concurrent user edit while the extension computes; inactive-tab policy; workspace switch; extension disable/restart; malformed-process failure without editor failure.

Before then, the test report MUST mark these checks **not run: CED extension host unavailable**, rather than passing them on the mock's behalf. Do not add or claim working CLI flags to bypass a missing loader.

## 14. Definition of done

An extension delivery contains:

- A package directory and manifest conforming to this specification, plus source and license text.
- Reproducible build/package commands and the supported OS/CPU matrix. Identify targets built/tested locally versus targets merely configured.
- An English README describing commands, arguments/results, requested capabilities, configuration, runtime requirements, installation availability, and how to disable the extension.
- Automated transformation, transport, lifecycle, and behavior tests appropriate to the feature, with actual execution results.
- An API compatibility statement naming this document revision, target API, tested host version/commit if any, and any draft assumptions or host gaps.
- A short test report separating mock conformance from real CED integration, plus known limitations and explicit handling of stale edits, cancellation, and unknown mutation outcomes.

Do not mark the task complete by producing source snippets alone. If host support is absent, the deliverable may be complete as a **draft-targeted extension package**, provided that limitation is clear and mock tests ran. It is not a verified extension for the current CED release.

### Copyable task brief for a coding agent

```text
Build a CED extension for: <requested behavior>.
Use docs/extension-authoring-spec.md revision 0.1.0-draft as the authoring contract.
First inspect the actual CED host/SDK and report implemented versus draft-only APIs.
Do not modify CED core unless host implementation is explicitly part of this task.
Use only defined manifest fields, advertised methods, and granted capabilities.
Implement framed bidirectional JSON-RPC with bounded queues and correct lifecycle.
Read unsaved text through documents/get; edit through one version-checked transaction.
Preserve invocation context, UTF-8 byte offsets, cancellation, and unknown-outcome rules.
Test the real extension executable against a mock host; run real-host checks if available.
Deliver a package, source, manifest, license, English README, build instructions, and report.
Do not claim CED integration from mock tests or invent missing APIs/CLI commands.
```

## 15. Maintenance and references

This is an authoring contract, not an implementation plan for CED internals. The [architecture document](arquitetura-extensoes.md) covers host component boundaries, ownership, CUI adaptation, and rollout phases. [README.md](../README.md) records current capabilities. [JSON-RPC 2.0](https://www.jsonrpc.org/specification) defines the underlying envelope and baseline errors; CED-specific restrictions and methods are defined here.

When changing a wire type, update every example, fixture, SDK binding, and affected host/extension test together. Minor API versions may add optional fields/methods; incompatible required fields, removed methods, or semantic changes require a major version once stable. Manifest format changes follow their own `manifestVersion`; package release versions and CED application versions remain independent. Until host validation and stabilization, advance the document revision and explicitly list compatibility changes rather than claiming API 1.0 stability.
