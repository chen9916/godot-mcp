# Godot MCP 0.3.0 release status

Updated: 2026-10-12. This document owns release scope and executed verification
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
The new release target with integrated Windows export templates is
`artifacts/Godot-MCP-0.3.0-Windows-x86_64-20261012`. It includes double-click
setup/connection-check launchers and the [five-step guide](MCP_QUICKSTART.md).
New OpenCode configurations use V2; existing V1/V2 configurations are preserved.
The previous verified distribution is
`artifacts/Godot-MCP-0.2.0-Windows-x86_64-20261007-templates`. The earlier
`artifacts/Godot-MCP-0.2.0-Windows-x86_64-20261007` bundle remains available without
export templates. Final payload checks run after
the source commit and rebuild; their results are recorded in the adjacent
`.verification.json` report. The ZIP and `.sha256` sidecar are produced only
after those checks pass.

The stdio MCP server identifies as 0.3.0. The lockfile uses MCP SDK 1.32.1,
and the selected bundled runtime is Node.js 22.23.1. Existing 2026-10-03 and
2026-10-05 distributions retain their original editor/bridge code and are
separate artifacts.

## Animation suite and easy setup in 0.3.0 (2026-10-12)

The current source adds nine animation tools (83 tools total): offline animation
schemas; library/clip/track/key inspection and atomic editing; nested
AnimationTree graph inspection, structural editing, and validation; and
owned-runtime animation playback, state-machine, parameter, one-shot, transition,
and root-motion inspection/control.

Animation editor patches require the active scene path/version, support dry runs,
edit local copies of animation resources, and commit one undo action. Saving is a
separate explicit operation. The bridge validates the new inputs against their
published schemas; native handlers also enforce engine-specific validation and
bounds. The editor advertises `animation_schema_version: 1` through session info.
See [animation tools](editor/debugger/godot_mcp/ANIMATION_TOOLS.md) for the full
contracts and examples. Windows packages include the runtime/bridge and all
matching Windows export templates, so users need no build tools or separate npm
installation. Setup remains opt-in and preserves unrelated client settings.

These source changes are not included in the existing 2026-10-07 archives or
export templates. A matching rebuilt editor/bridge and an MCP reconnect are
required; the user's existing running editor is not automatically replaced.
The current isolated verification editor is
`bin/godot.windows.editor.x86_64.mcp_controller_verify.exe`; the earlier
`mcp_latest_verify` executable is preserved, including any running editor using
it. Ordinary non-`@tool` AnimationTree controller scripts now retain their script
references and exported values during graph edits, parameter inspection, dry
runs, and undo/redo. Editor-executing tree scripts and custom scripted graph
resources remain restricted. The matching source bridge
is built with `npm.cmd run build` in `misc/godot_mcp_bridge/`. Native verification
is reproducible with `misc/godot_mcp_bridge/scripts/run-native-tests.mjs`, which
copies the binary and fixtures into an isolated profile and retains evidence logs.

| Animation verification | Current result |
| --- | --- |
| Bridge default suite | 89 passed, 6 optional live workflows skipped; 95 total, including new OpenCode V2 defaults and legacy-preservation regressions. |
| Isolated animation/protocol workflows | Passed on the controller-support verification editor; both are required again against the packaged editor by the 0.3.0 release gate. |
| Dependency audit | 0 reported vulnerabilities on 2026-10-12. |
| Portable ZIP tooling probe | Passed CRC/hash/checksum round trip; tampering and failed release gates rejected before creating an archive. |
| Standard SDK schemas/discovery | Passed, including offline schemas and malformed-input rejection across four protocol versions. |
| Windows runtime, blend-space, and Transition objects, `werror=yes` | Passed in editor/debug-template/release-template configurations, including lifetime, typed-integer, manual-triangle reload, and per-instance Transition initialization fixes. |
| Native MCP and UndoRedo regressions | 90 MCP tests / 2,234 assertions and 3 UndoRedo tests / 51 assertions passed in an isolated installation, with no unexpected diagnostics. Includes 52 animation graph/library regressions, ordinary controller preservation, and rejection of editor-executing tree scripts before property hooks. |
| Isolated native protocol workflow | Passed ownership, resource lifetime, UTF-8 framing, and GDScript query regressions. |
| Complete Windows editor build, `werror=yes` | Passed with ordinary controller-script authoring, reviewed graph/library fixes, lexical scene-version ordering, manual-triangle reload, and per-instance Transition initialization. A temporary SCons profile redirected only the editor output, reusing the verified object configuration without overwriting the running editor. |
| Isolated animation editor/runtime workflow | Passed all 16 runtime actions, controller-script/export preservation through graph authoring and packing, controller execution in the owned runtime, nested/automatic state transitions, one-shot and transition requests, typed keys/parameters, bounded root-motion snapshots, parameter-preserving rename, ordinary undo/redo, save/reload, freed-reference handling, pause/resume, and stale-run rejection. No native or script diagnostics. |

The packaged 0.3.0 release gate requires native regressions, package inventory
and source hashes, exact 83-tool discovery, animation/controller authoring and
runtime checks, double-click setup/doctor launchers, editor setup, ordinary
release workflows, and actual Windows debug/release exports. The final executed
outcome is recorded in the new archive's `.verification.json`, not inferred from
the older evidence below. The GitHub release is a Windows MCP distribution of a
custom development engine, not an official stable Godot release.

The original packaged-release evidence below remains dated 2026-10-07; it does
not establish verification of the new source suite or a new distribution.

## Windows export templates

The template distribution includes matching Windows x86_64 debug and release
executables and their console launchers. They are installed in the portable
editor's `editor_data/export_templates/4.8.dev/` directory, so ordinary Windows
Desktop export presets can find them without custom template paths. Direct3D 12
Agility runtime DLLs and their original license accompany the templates.

The build uses `tests=no`, `debug_symbols=no`, `accesskit=no`, `angle=no`, and
`werror=yes` for both template targets. The editor and templates must report the
same full build version. Release templates do not initialize the debug-only MCP
runtime integration. A standalone Windows `.tpz` installer and checksum are also
produced from the verified package.

`scripts/verify-windows-export.mjs` copies the portable bundle into a disposable
installation, exports a small project using the default installed templates, and
runs debug/release games and their console launchers. The archive verification
report records the actual result after packaging. The check exercises standalone
headless execution; it does not certify every graphics driver, third-party
extension, signing configuration, or game project.

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
| Windows debug/release exports and standalone console/GUI binaries | Required for the template distribution; exact results are recorded by the Windows export verifier in the archive verification report. |
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
graphics, configuration, and package checks. Windows x86_64 templates and the
standalone export smoke test are included in the template distribution; templates
for other platforms and validation of individual game projects are outside its
scope.
