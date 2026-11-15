# Text Tools native extension

An executable C99 example for the experimental CED host. No scripting runtime or SDK installation is required.

Build CED normally with `make`. CMake configures the current platform's manifest and builds the executable in `build/examples/extensions/uppercase-selection/bin/`. The source manifest is a CMake template: install the **generated package folder**, not this source folder. Changing `BUILD_DIR` changes that package path accordingly. Nothing is installed automatically.

If you rebuild an already installed example version, uninstall it before reinstalling: approving different contents under the same registered version is intentionally rejected. The generated package includes cJSON's license under `licenses/`.

1. In CED, use **Install Extension from Folder** in the command palette and select the generated folder.
2. Review the package and allow its `documents.read` and `documents.edit` grants.
3. In **Manage Extensions**, trust your workspace.
4. Open a workspace text file, select text and run **Uppercase ASCII Selection** in the palette. For an untitled/outside document, grant its active document explicitly in the manager first.
5. Undo/redo the edit normally. The file remains unsaved.

Command ID: `example.text-tools.uppercase`. Arguments: `null`. It transforms ASCII `a`–`z` to `A`–`Z`, preserves every other UTF-8 scalar, keeps the selection direction, and returns the apply result `{documentId, version, changed}`. Already-uppercase input is a no-op with no history entry. It reads the invocation's captured document/selection, verifies snapshot revision, and supplies `expectedSelectionVersion` before editing; it never switches to a different active document or retries an unsuccessful mutation. A failed host service is forwarded as a terminal command error with its original code, reason and data (including `currentVersion`). A locally detected stale snapshot also returns `StaleDocument`. Cancellation, EOF and deactivation stop pending work. An interrupted apply has an unknown outcome: check the buffer rather than retrying blindly.

Communication uses framed JSON-RPC on stdin/stdout; never print diagnostics to stdout. This example only requires `documents/get` and `documents/applyEdits`, with initialization/activation and explicit deactivation handling. CED's [native integration test](../../../tests/extension_integration_test.c) runs this exact executable against real host services and the CUI edit adapter.
