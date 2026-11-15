# Third-party code

Dependencies are checked out as Git submodules. Initialize them with `git submodule update --init --recursive`; the parent repository's Git references pin their exact commits.

| Dependency | Submodule | Pinned revision | License |
| --- | --- | --- | --- |
| [CUI](https://github.com/yappologistic/CUI) | `third_party/CUI` | `6e6b35326f655405822fcf863fac0d3bb35bb787` | MIT, `LICENSE` |
| [cJSON](https://github.com/DaveGamble/cJSON) | `third_party/cjson` | v1.7.19, `c859b25da02955fef659d658b8f324b5cde87be3` | MIT, `LICENSE` |
| [libvterm](https://github.com/neovim/libvterm) | `third_party/libvterm` | v0.3.3, `9d6d2112335080312ef8c36667fa717ded4f7daf` | MIT, `LICENSE` |

The terminal and editor-assistance adapters use internal CUI APIs, so its revision is pinned. libvterm comes from Neovim's Git mirror of [upstream](https://www.leonerd.org.uk/code/libvterm/); the pinned source files match the original 0.3.3 release. Its encoding headers are generated from the upstream tables and Perl script into the build directory.

clangd and the user's shell run as separate local processes and are not bundled with CED.
