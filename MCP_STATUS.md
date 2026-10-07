# Godot MCP 0.2.0 release status

Updated: 2026-10-07. This document owns release scope and executed verification
evidence. Setup, contracts, architecture, and reproducible commands live in the
[bridge guide](misc/godot_mcp_bridge/README.md),
[engine guide](editor/debugger/godot_mcp/README.md),
[tool reference](editor/debugger/godot_mcp/AUTOMATION_TOOLS_V2.md),
[architecture](editor/debugger/godot_mcp/ARCHITECTURE.md), and
[test guide](misc/godot_mcp_bridge/test/README.md).

## Release scope

The target is the local Godot MCP editor/runtime integration on Windows x86_64,
distributed with the standard stdio bridge, Node runtime, locked production
dependencies, and license notices. The engine is a custom Godot 4.8-dev build.
The 0.2.0 distribution path is
`artifacts/Godot-MCP-0.2.0-Windows-x86_64-20261007`. Final payload checks run after
the source commit and rebuild; their results are recorded in the adjacent
`.verification.json` report. The ZIP and `.sha256` sidecar are produced only
after those checks pass.

The stdio MCP server identifies as 0.2.0. The lockfile now uses MCP SDK 1.32.1,
and the selected bundled runtime is Node.js 22.23.1. Existing 2026-10-03 and
2026-10-05 distributions retain their original editor/bridge code and are
separate artifacts.

## Changes in 0.2.0

- Cancellation reaches discovery, pending RPC calls, reconnect backoff, and
  filesystem/runtime polling. Canceling one caller preserves another caller's
  shared discovery; cancelled discovery cannot later dispatch a mutation.
  Runtime/filesystem operation deadlines include discovery, outstanding RPCs,
  polling delays, and acknowledgments. A completed result survives a failed
  acknowledgment, and uncertain operations are not acknowledged as complete.
- TCP decoding preserves split multibyte UTF-8 in both directions and resets
  framing state after reconnect. The editor enforces its 1 MiB request limit in
  bytes instead of decoded character count.
- Editor node lookup remains inside the edited scene, including normalized
  relative paths. Reparent, delete, and batch-delete retain subtree ownership
  through undo/redo and serialization; equivalent batch targets are rejected.
  Scene-patch sibling positions and bounds exclude internal children.
- Newly created resources stay available until saved, with a 128-resource
  pending limit and conflict checks for existing or externally created files.
  Creation and property updates to pending resources reject `resource_path`
  retargeting. Resource access/save paths must remain canonical within `res://`.
  Scene extensions are reserved for `scene.create` so creation cannot produce
  a pending resource that the scene-save route cannot persist.
- GDScript queries use the current editor-language API and enabled-module
  detection. Definitions, hover, symbols, and references have live regression
  coverage available. Reference search tokenizes identifiers, excludes comments
  and strings, respects declaration filtering, and reports approximation and
  truncation explicitly.
- Runtime mouse events carry global positions and the held button mask, with
  buffered events flushed around injection. A real press/drag/release sequence
  is included in the runtime workflow.
- Portable discovery reads the matching self-contained editor's recent-project
  registry before the normal user registry. Explicit project/session pins and
  project working-directory discovery retain precedence. Doctor returns exit 1
  for configuration/startup failures and exit 2 for a healthy bridge without an
  available editor.
- The setup workflow copies current bridge outputs and locked production
  runtime files with bounded concurrency, reports preparation/launch stages,
  and waits for its owned editor to close before removing temporary files.
  Dependency updates resolve the vulnerabilities reported by the current npm
  audit.

## Preserved guarantees

Scene patches continue to plan Node3D/Node2D global properties against the
planned hierarchy, store local backing values, and restore exact stored state
on undo/redo. Unsupported Control conversions and singular parent inverses are
rejected. Ordered/repeated property entries and script-before-property assignment
are retained. Failed planning and embedded-script deletion restore stored script
fields. Best-effort planning isolates independent operations and reports skipped
dependents in order.

Scene saves use a path-bound loaded/last-saved content baseline, including
preserved timestamps, deletion/replacement, and pre-save callbacks. Runtime
ownership, stale session/run guards, paused stepping, held input, viewport
capture, and run-filtered diagnostics remain part of the release contract.
The isolated release workflow exercises these behaviors against the rebuilt
editor; final payload checks are recorded separately as described above.

Shared setup remains available for Codex, OpenCode V1/V2, Hermes, Antigravity,
Claude Code, and Claude Desktop. It preserves unrelated settings, refuses
malformed/ambiguous files, creates private backups where supported, and detects
concurrent content changes before replacing configuration. It provides config
export, client-path listing, removal, and diagnostics without requiring changes
to normal agent profiles during tests.

## Verification on 2026-10-07

| Check | Current result |
| --- | --- |
| TypeScript typecheck and production/test compilation | Passed. |
| Bridge default suite | 80 passed, 5 optional live workflows skipped; 85 total. |
| Independent cancellation/deadline, waiter, RPC, and lifecycle gate | 30 passed, including actual SDK cancellation and uncertain acknowledgment handling. |
| Focused discovery/setup/doctor gate | 10 passed using disposable files. |
| `npm audit` with the updated lockfile | 0 reported vulnerabilities. |
| Windows MSVC editor with `tests=yes`, `werror=yes` | Passed, including follow-up resource/path guards. |
| Native MCP and core UndoRedo tests on the rebuilt editor | 38 MCP tests / 898 assertions and 3 UndoRedo tests / 51 assertions passed. |
| Settings/setup workflow | Passed against the rebuilt editor: portable, moved portable, source layout, unpinned registration, and independent SDK connection. |
| Native protocol workflow | Passed: containment, owner undo/redo and persistence, resources and alias/capacity/conflict guards, UTF-8 request bytes, and all four GDScript queries. |
| Isolated release workflow | Passed: real unsaved buffers, save conflicts, scene patches, runtime, injected mouse coordinates/masks, capture, and diagnostics. |
| Final package inventory/hashes, SDK smoke, restart/reconnect, moved payload | Required after clean-source packaging; final outcome is in the archive verification report. |
| Clean committed source/manifest match, ZIP, and checksum | Required after committing and rebuilding; source identity is in `RELEASE_MANIFEST.json`, archive evidence in the verification report. |

Tests which require localhost and atomic replacement must have that access.
Failures caused by a restricted temporary-file sandbox are rerun with the
required access and do not establish a product regression. Disposable projects,
settings, and configuration files are used throughout. Normal client
configurations and pre-existing `artifacts/` data are preserved.

## Release gate and reproduction

Use the test guide to build the separate verification executable, run native
tests, and run the live workflows serially. Build the final source revision
before generating a new package. Run the package smoke verifier and complete
isolated release workflow against that payload before creating its ZIP and
checksum. `verify-package.mjs --check-source` additionally requires the exact
committed revision and a clean checkout. A dirty tooling probe records that
state and does not satisfy this source gate.

The source checks above are independent of final package verification. A release
requires both; a directory without its successful verification report is not a
validated distribution.

## Support boundaries

Cancellation and timeout stop the caller's wait; an operation already sent to
the editor/runtime may still complete. Do not infer rollback from either result.
Unsafe mutations are not replayed after an ambiguous disconnect. Deduplicated
runtime requests have their separate acknowledgment/retention contract.

Scene content checks are optimistic comparisons without an operating-system
file lock; an unrelated writer can race the final check. Non-scene resource
saves retain their timestamp guard, plus existence checks for pending created
resources. Pending creations belong to an editor MCP session and are bounded;
they must be saved before ending that session. Script undo restores stored
properties, not arbitrary non-stored script state. Selected custom setters and
mutation sequences are covered rather than every possible script combination.

GDScript reference locations are approximate lexical matches, not a complete
semantic reference index. Search caps locations and source size and reports
truncation; callers must inspect those flags. Internal editor children are
excluded from public scene-patch positions. Mouse injection does not warp the
desktop cursor; root-viewport mouse-position polling can still read that physical
cursor. Coverage exercises a selected press/drag/release sequence, not every
platform/window/input-routing combination.

Client fixtures and independent MCP initialization establish format and launch
behavior, not that every running agent application has reloaded its tools.
Support beyond Windows x86_64 requires the corresponding native, live-editor,
graphics, configuration, and package checks. Godot export templates and exported
game validation are outside this MCP editor distribution.
