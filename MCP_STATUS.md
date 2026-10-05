# Godot MCP release status

Last checked: 2026-10-05 for shared setup; native release evidence below is from
2026-10-03. This document owns release scope and verification
evidence. Setup, API contracts, architecture, and reproducible checks live in the
[bridge guide](misc/godot_mcp_bridge/README.md),
[engine guide](editor/debugger/godot_mcp/README.md),
[tool reference](editor/debugger/godot_mcp/AUTOMATION_TOOLS_V2.md),
[architecture](editor/debugger/godot_mcp/ARCHITECTURE.md), and
[test guide](misc/godot_mcp_bridge/test/README.md).

## Release scope

The release target is the local MCP editor/runtime integration on Windows
x86_64, packaged with its stdio bridge, Node runtime, locked production
dependencies, and licenses. The underlying engine is a custom Godot 4.8-dev
build. These checks do not establish an upstream stable Godot release or support
for untested platforms and every MCP client application.

The Windows MCP implementation and release checks below are complete. The prior
distribution is `artifacts/Godot-MCP-4.8-dev-Windows-x86_64-20261003`, with a
matching ZIP and SHA-256 sidecar. Its `RELEASE_MANIFEST.json` records the exact
source commit, embedded editor version, Node version, and source/payload hashes.
Pre-existing distributions are separate artifacts.

## Shared client setup (2026-10-05)

The bridge now provides a common `godot-mcp setup` flow for Codex, OpenCode,
Hermes, Antigravity, Claude Code, and Claude Desktop. It selects clients, resolves
their native configuration paths, preserves unrelated settings, backs up changed
files, and exposes `config`, `clients`, `remove`, and `doctor` commands. Windows
packaging includes a launcher and Node runtime. See the bridge guide for scopes,
environment overrides, OpenCode V1/V2, and alternate configuration paths.

TypeScript typecheck and production build passed. The updated default suite
passed 61 tests, with four optional live workflows skipped. A separate focused
review found no remaining setup blockers. Repository checks passed, as did an
isolated npm tarball installation and its generated command launcher. This pass changes bridge setup and
packaging only; it reuses the previously verified editor binary. Client fixtures
and independent MCP diagnostics establish configuration and launch behavior;
they do not establish that every agent application has reloaded its tools.

The 2026-10-03 archive retains its original bridge. Generate a new distribution
with the updated packager and run its package verifier before using shared setup.

## Correctness work

- [x] Plan Node3D and Node2D global properties against the planned hierarchy;
  write local backing properties and restore exact stored state on undo/redo.
  Reject unsupported Control-based conversions and singular parent inverses.
- [x] Preserve ordered and repeated `property_entries`, attach scripts first in
  object-form assignments, and restore stored script fields during failed
  planning and undo/redo.
- [x] Rebuild best-effort plans after final NodePath conflicts, retaining
  independent operations and skipping dependent operations with ordered results.
- [x] Protect scene saves with a path-bound loaded/last-saved content baseline,
  including preserved timestamps, deletion/replacement, and pre-save callbacks.
- [x] Preserve result arrays after planner destruction and restore script fields
  after failed embedded-script deletion.
- [x] Verify runtime startup callbacks, paused stepping, held input, runtime
  writes/calls, viewport capture, and diagnostics across separate runs.

## Verification

| Check | Recorded result |
| --- | --- |
| Windows MSVC editor build with `tests=yes`, `werror=yes` | Passed. |
| Native MCP regressions | 37 tests, 834 assertions passed. |
| Native core UndoRedo regressions | 3 tests, 51 assertions passed. |
| Bridge default suite | 31 passed; 4 live workflows skipped by default. |
| TypeScript typecheck and production bridge build | Passed. |
| Repository pre-commit checks on changed files | Passed. |
| Real unsaved script tab/scene against preserved-time edits and deletion | Passed in isolated editors. |
| Disk change during a scene pre-save callback | Passed; committed memory changes report a save conflict and preserve external disk contents. |
| Live scene patch persistence/reload | Passed in the isolated release workflow. |
| Complete live runtime workflow | Passed: validation, SVG import/load, reload, two owned runs, inspection, property writes/calls, exact paused process/physics steps, held input, resume, capture, and required diagnostic markers. |
| Isolated editor configuration export | Passed: settings controls, portable/source layouts, independent SDK connection, and moved-folder recovery. |
| Actual release payload, independent SDK, restart/reconnect, moved folder | Passed using disposable package copies; production inventory and hashes verified. The final package also requires a clean source/manifest match. |

Test projects and editor settings are disposable and self-contained. Normal
client configurations and pre-existing archives under `artifacts/` are preserved.

## Release reproduction

Commit the organized source, rebuild that revision, and generate a new package
with the commands in the test guide. Run `verify-package.mjs --check-source` and
the complete isolated release workflow against that bundle before creating its
ZIP and checksum. The source check requires the exact committed revision and a
clean working tree. The final distribution produced for this release follows
this sequence.

No reproduced correctness blocker remains open in the tested Windows MCP scope.
Additional platform/client coverage and the boundaries below remain outside that
scope.

## Support boundaries

Scene content checks are optimistic comparisons without an operating-system
file lock; an unrelated writer can race the final check. Non-scene
`resource.save` retains its timestamp guard. Script replacement undo restores
stored properties, not arbitrary non-stored script state. Native and live
regressions cover selected custom setters and mutation combinations, not every
possible script.

To extend this release beyond Windows x86_64, build and repeat the native,
live-editor, graphics-capture, configuration-export, and package checks on each
additional platform. Godot export templates and game-export validation are
outside this MCP editor distribution. No calendar estimate or completion
percentage is asserted.
