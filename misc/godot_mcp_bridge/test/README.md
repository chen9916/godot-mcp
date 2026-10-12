# Godot MCP build and test guide

These commands describe available checks. See [MCP status](../../../MCP_STATUS.md) for recorded verification, supported release scope, and remaining work.

## Bridge build and default tests

From `misc/godot_mcp_bridge`, with Node.js 18 or later:

```powershell
npm.cmd ci
npm.cmd run typecheck
npm.cmd test
npm.cmd run build
npm.cmd audit
```

On platforms without `npm.cmd`, use `npm`. `npm test` compiles `.test-dist` and runs unit, localhost transport, independent stdio, and configuration-export tests. Its runner selects outputs corresponding to current test sources, excluding stale outputs from deleted or renamed tests. It does not require Godot unless optional workflow environment variables are set. The six live workflows below are skipped by default; a successful default run therefore leaves those workflows unverified.

The lockfile uses MCP SDK 1.32.1; the selected Windows release runtime is Node.js
22.23.1. Executed test counts and audit results are recorded in the status
document, separately from the native/live checks and final package evidence.

Transport tests split multibyte UTF-8 across response chunks and reset decoder
state on reconnect. Unit and independent SDK fixtures check cancellation during
discovery and polling, one cancelled caller alongside a surviving discovery
caller, pending RPC cleanup, late responses, and operation deadlines shorter
than the configured RPC timeout. Runtime waiter checks cover stable request IDs,
arbitrary call arguments, and failed acknowledgments of completed results.

The default suite also checks shared client setup for Codex, OpenCode V1/V2,
Hermes, Antigravity, Claude Code, and Claude Desktop using isolated files. It
exercises comment/settings preservation, malformed and ambiguous documents,
environment and scope paths, disabled registrations, repeated setup, removal,
backups, concurrent edits, and CLI subprocesses. Doctor tests initialize the real
bridge against offline and fixture editor endpoints and verify bounded process
termination. They do not install into the user's agent profiles.
Discovery fixtures cover both portable markers, matching portable/source
registries, global fallback, and explicit pin/working-directory precedence.
Doctor CLI fixtures distinguish startup failures (exit 1) from a healthy bridge
with no editor (exit 2).

## Native editor build and tests

Godot-side tests in `tests/editor/godot_mcp/` are included when building with `tests=yes`. The following Windows command uses the sibling SCons checkout provided in this workspace. Run from the engine root with the required Godot Windows build dependencies installed:

```powershell
$env:PYTHONPATH = (Resolve-Path "../scons").Path
python -m SCons platform=windows target=editor tests=yes extra_suffix=mcp_release_0_3_0 werror=yes debug_symbols=no accesskit=no angle=no -j12 bin/godot.windows.editor.x86_64.mcp_release_0_3_0.exe bin/godot.windows.editor.x86_64.mcp_release_0_3_0.console.exe
node misc/godot_mcp_bridge/scripts/run-native-tests.mjs (Resolve-Path "./bin/godot.windows.editor.x86_64.mcp_release_0_3_0.exe").Path
```

The explicit executable target creates a separate verification binary and avoids
the default targets which copy optional DLLs beside a possibly running editor.
It also builds the matching console companion. Native test fixtures resolve relative to
the executable at `../tests/data`, so a copied executable needs that fixture
layout too. The runner creates that disposable layout, places `_sc_` beside its
copied binary, and redirects profile, user-data, and temporary directories. It
runs `[GodotMCP]` and `[UndoRedo]` separately, enforces an owned-process timeout,
and rejects unexpected error/warning diagnostics. Logs and small fixtures are
retained at the reported path; the copied binary is removed. Other platforms
require their corresponding build options and binary name.

## Live runtime workflow

Use a disposable project containing `project.godot`, open it in the rebuilt editor, enable `network/godot_mcp/enabled`, and close game runs belonging to that test project. From the bridge directory:

```powershell
npm.cmd run build:test
$env:GODOT_MCP_INTEGRATION = "1"
$env:GODOT_MCP_INTEGRATION_PROJECT = "C:\absolute\path\to\disposable-test-project"
node --test .test-dist/test/editor_integration.test.js
```

The test validates disk and supplied invalid GDScript, creates/scans/imports an SVG, reloads a changed disk script, reads partial settings, starts two game runs, inspects and writes runtime properties, calls a method, checks paused physics/process stepping and held input, resumes, checks a mouse press/drag/release sequence with event masks and global positions, captures a viewport, and checks run-filtered diagnostics. It refuses to overwrite an existing fixture directory. An `after` hook stops its owned run, removes its fixture and captured PNG, scans the filesystem, and closes the bridge connection.

## Live scene-patch workflow

This workflow uses the same disposable-project setting but a separate flag and test process. Run it separately from the runtime workflow because both change editor scene/run state:

```powershell
npm.cmd run build:test
$env:GODOT_MCP_SCENE_PATCH_INTEGRATION = "1"
$env:GODOT_MCP_INTEGRATION_PROJECT = "C:\absolute\path\to\disposable-test-project"
node --test .test-dist/test/scene_patch_integration.test.js
```

It exercises root rename dry runs/commits, subtree move/save, JSON integer positions and packed arrays, sibling collision rejection, and returned paths after property-based renames. It creates a unique fixture directory and removes it afterward. Native scene-patch tests separately cover undo/redo, snapshots, packing, and SceneTree ownership.

## Isolated animation workflow

This workflow copies a rebuilt editor into a uniquely owned self-contained
installation and authors animation resources in a disposable project. It uses
the actual standard MCP bridge for discovery, clip/track/key and graph patches,
validation, guarded rollback/dry runs, persistence, and owned-runtime animation
playback. It never changes an existing user's project or running editor.

```powershell
npm.cmd run build
npm.cmd run build:test
$env:GODOT_MCP_ANIMATION_EDITOR = "C:\absolute\path\to\rebuilt\Godot.exe"
node --test .test-dist/test/animation_integration.test.js
```

Clear `GODOT_MCP_ANIMATION_EDITOR` before running the default bridge suite. On
Windows the animation fixture uses the approved OpenCode temporary directory.
Native animation tests are included in the ordinary `[GodotMCP]` test filter.
The workflow keeps an ordinary non-`@tool` controller script attached to the
AnimationTree throughout authoring. It checks script/export preservation through
packing, dry runs, actual editor-menu undo/redo, and save/reload, and calls the
preserved controller in the owned runtime. It also checks typed Dictionary and
packed integer/vector reinsertion, graph parameter preservation on rename,
automatic and requested state transitions, all 16 playback actions, bounded
snapshots, freed-reference serialization, and stale-run rejection. Profile,
user-data, logs, and temporary paths are redirected for both owned processes.
It also verifies runtime pause/resume and fails on unexpected editor/runtime
error or warning diagnostics. The default suite skips this workflow; setting the
editor variable opts into execution rather than treating skipped cases as proof.

## Isolated editor setup workflow

On Windows, this workflow makes a temporary self-contained bundle and project using the selected rebuilt executable, current Node executable, current-source bridge outputs, and runtime files from locked production dependencies. It omits development tools, declarations, and source maps and bounds parallel file copies. It launches its own headless editor and inspects Editor Settings controls through an editor plugin:

```powershell
npm.cmd run build
npm.cmd run build:test
$env:GODOT_MCP_SETUP_EDITOR = "C:\absolute\path\to\rebuilt\Godot.exe"
node --test .test-dist/test/editor_setup_integration.test.js
```

It checks the Godot MCP category/search with localization on/off, config-button export, enabled session, repeat export, missing-bridge recovery, moved bundle paths, and source-checkout discovery with Node on PATH. It registers an unpinned portable bridge in a disposable client file and checks discovery from outside the project using the portable registry. An independent SDK client launches the exported connection and checks tool discovery and the live session handshake in all three layouts. It does not launch a coding agent or install configuration into an agent application. Diagnostics identify preparation and launch stages, and cancellation stops the owned editor before temporary files are removed.

## Isolated release workflow

This Windows test creates disposable projects and copies the selected editor or
release bundle. It opens a real script tab, edits its buffer and scene without
saving, changes disk contents while preserving timestamps, and checks that reload
and save refuse conflicts without losing either version. A separate project
checks disk writes during pre-save callbacks. Fresh projects then run the live
scene-patch and runtime workflows serially, including viewport capture and actual
run diagnostic markers.

```powershell
npm.cmd run build:test
$env:GODOT_MCP_RELEASE_EDITOR = "C:\absolute\path\to\rebuilt\Godot.exe"
node --test .test-dist/test/release_editor_integration.test.js
```

To check a release payload, set `GODOT_MCP_RELEASE_BUNDLE` to its directory instead
of `GODOT_MCP_RELEASE_EDITOR`. The test copies that bundle before launching it.
The runtime capture phase requires a working graphics driver; the editor itself
runs headless. Cleanup stops only owned processes before removing temporary data.

When finished, clear optional flags before running the default suite:

```powershell
Remove-Item Env:GODOT_MCP_INTEGRATION, Env:GODOT_MCP_SCENE_PATCH_INTEGRATION, Env:GODOT_MCP_INTEGRATION_PROJECT, Env:GODOT_MCP_SETUP_EDITOR, Env:GODOT_MCP_RELEASE_EDITOR, Env:GODOT_MCP_RELEASE_BUNDLE, Env:GODOT_MCP_PROTOCOL_EDITOR, Env:GODOT_MCP_ANIMATION_EDITOR -ErrorAction SilentlyContinue
```

## Isolated native protocol regressions

This workflow copies the selected rebuilt editor into a temporary self-contained
installation, redirects profile/user-data/temporary paths, and runs actual
endpoint requests. Fixture commands use atomic file publication so concurrent
tests cannot expose partial JSON to the editor. It verifies scene containment,
reparent/delete/batch-delete ownership across undo/redo and save/reload, retained
resource creation and its capacity/conflict guards, canonical resource paths,
split UTF-8 requests, the request byte limit, and all four GDScript LSP operations.
Reference assertions exclude comments, strings, multiline text, and longer
Unicode identifiers and cover declaration filtering, including a declaration
on the first source line. These are lexical matches with `approximate: true`,
not a semantic reference index; callers must also inspect `truncated`.

```powershell
npm.cmd run build:test
$env:GODOT_MCP_PROTOCOL_EDITOR = "C:\absolute\path\to\rebuilt\Godot.exe"
node --test .test-dist/test/native_protocol_integration.test.js
```

Clear `GODOT_MCP_PROTOCOL_EDITOR` before the default suite. Like the other live
workflows, this check requires localhost connections and atomic file replacement
in its disposable directories. Sandboxes which deny either must run it with the
required access; permission failures do not establish a product regression.

## Windows packaging and package verification

Commit the final source and build its editor before packaging. The packager
requires a clean checkout and a new output directory under `artifacts/`; it
refuses to overwrite an existing package. Supply the complete license from the
selected Node distribution. Run from the bridge directory:

```powershell
$mcpReleaseDirectory = [System.IO.Path]::GetFullPath("../../artifacts/Godot-MCP-0.3.0-Windows-x86_64-20261012")
$mcpReleaseEditor = [System.IO.Path]::GetFullPath("../../bin/godot.windows.editor.x86_64.mcp_release_0_3_0.exe")
$mcpReleaseConsole = [System.IO.Path]::GetFullPath("../../bin/godot.windows.editor.x86_64.mcp_release_0_3_0.console.exe")
# Optional editor-only package. For the ready-to-ship release use the template command below instead.
$mcpEditorOnlyDirectory = "$mcpReleaseDirectory-editor-only"
npm.cmd run package:windows -- --editor "$mcpReleaseEditor" --editor-console "$mcpReleaseConsole" --output "$mcpEditorOnlyDirectory" --node-license "C:\absolute\path\to\Node\LICENSE"
node scripts/verify-package.mjs --package "$mcpEditorOnlyDirectory" --check-source
$env:GODOT_MCP_RELEASE_BUNDLE = $mcpEditorOnlyDirectory
node --test .test-dist/test/release_editor_integration.test.js
```

The payload contains the editor, `godot-mcp.cmd`, double-click `Setup.cmd`,
`Setup-OpenCode.cmd`, `Check-Connection.cmd`, a quick-start guide, bridge build, bundled Node, lockfile-installed
production dependencies, and notices. Its manifest records the source commit,
source and payload hashes, and executable versions. `--check-source` verifies
that the clean checkout still matches that manifest. The smoke verifier checks
the actual launcher with quoted paths, setup/doctor against a disposable client
file, the double-click OpenCode setup/doctor wrappers using a disposable V2
configuration, exact tool discovery and animation-suite negotiation, an
independent SDK client, scene mutation/save, saved state across an editor
restart, reconnecting the same bridge, and exporting/connecting after moving a
temporary copy. Packaging currently supports Windows x86_64.
Every current bridge source module contributes its compiled JavaScript file,
including `request_control.js`; stale dist files are excluded. Source hashes
include bridge source, tests, and packaging scripts. `--allow-dirty` is reserved
for a recorded tooling probe and cannot satisfy the clean source release gate.

To include Windows export templates, build both `template_debug` and
`template_release` targets for x86_64 from the same committed revision and build
configuration as the editor, including their console companions. Supply all four
inputs together; partial pairs are rejected before creating an output directory.
Use a fresh output path, for example:

```powershell
$mcpTemplateReleaseDirectory = $mcpReleaseDirectory
npm.cmd run package:windows -- --editor "$mcpReleaseEditor" --editor-console "$mcpReleaseConsole" --output "$mcpTemplateReleaseDirectory" --node-license "C:\absolute\path\to\Node\LICENSE" --template-debug "C:\absolute\path\to\template_debug.exe" --template-release "C:\absolute\path\to\template_release.exe" --template-debug-console "C:\absolute\path\to\template_debug.console.exe" --template-release-console "C:\absolute\path\to\template_release.console.exe"
node scripts/verify-package.mjs --package "$mcpTemplateReleaseDirectory" --check-source
node scripts/verify-windows-export.mjs --package "$mcpTemplateReleaseDirectory"
```

Every executable must be a Windows x86_64 PE file. Both main template `--version`
strings must exactly match the editor, including its build and commit identity.
The portable paths are
`editor_data/export_templates/<GODOT_VERSION_FULL_CONFIG>/windows_debug_x86_64.exe`
and `windows_release_x86_64.exe`, with corresponding `_console.exe` companions.
The current configuration directory is `4.8.dev`; the packager derives it from
the actual editor version and validates it against source version definitions.
The optional `export_templates` manifest metadata records the configuration,
build, exact editor version, and all four hashed paths. Packages without this
metadata retain the editor-only verification path.

For templates built with Direct3D 12, additionally pass
`--template-d3d12-core <D3D12Core.dll>`,
`--template-d3d12-layers <d3d12SDKLayers.dll>`, and
`--template-d3d12-license <Agility SDK LICENSE.txt>` together. The DLLs must be
x86_64 and match the SDK used by the build. They are installed as
`D3D12Core.x86_64.dll` and `d3d12SDKLayers.x86_64.dll` beside the templates, with
matching `D3D12Core.dll` and `d3d12SDKLayers.dll` beside the editor, and
`D3D12-LICENSE.txt` preserving the distribution terms in both locations. The optional
`export_templates.runtime_files` map records their hashed paths. This group
requires the four template inputs and rejects missing DLLs or license notices.

The Windows export verifier copies the payload into a disposable directory,
imports a small project, exports debug and release presets with empty custom
template paths, and runs both main outputs and console companions. This checks
automatic template discovery and actual exported-game execution. It does not
establish support for other platforms or every renderer/project combination.

For the current Windows build, these commands run from the engine root and
produce both template executables and their `.console.exe` companions. Match
the editor's remaining build options and revision; template builds can use
`tests=no` while retaining debugging features in `template_debug`:

```powershell
scons platform=windows target=template_debug arch=x86_64 accesskit=no angle=no debug_symbols=no tests=no werror=yes extra_suffix=mcp_release_0_3_0 bin/godot.windows.template_debug.x86_64.mcp_release_0_3_0.exe bin/godot.windows.template_debug.x86_64.mcp_release_0_3_0.console.exe
scons platform=windows target=template_release arch=x86_64 accesskit=no angle=no debug_symbols=no tests=no werror=yes extra_suffix=mcp_release_0_3_0 bin/godot.windows.template_release.x86_64.mcp_release_0_3_0.exe bin/godot.windows.template_release.x86_64.mcp_release_0_3_0.console.exe
```

After packaging and both verifiers above pass, create a standalone TPZ from the
validated bundle. Run from the bridge directory with a new output path:

```powershell
$mcpTemplateArchive = [System.IO.Path]::GetFullPath("../../artifacts/Godot-MCP-Windows-x86_64-templates.tpz")
python scripts/package-windows-templates.py --package "$mcpTemplateReleaseDirectory" --output "$mcpTemplateArchive"
Get-FileHash "$mcpTemplateArchive" -Algorithm SHA256
Get-Content "$mcpTemplateArchive.sha256"
```

The builder requires a clean release manifest and verifies the template and
notice hashes before writing. It includes `templates/version.txt`, all four
standard executable names, Godot notices, and the declared D3D12 files and license
when present. It verifies archive CRCs, inventory, and each entry's SHA-256, then
writes an adjacent `.tpz.sha256`; existing outputs are refused. Fixed metadata
and level-six DEFLATE reproduce identical inputs with the same Python/zlib
runtime. Install the TPZ through **Manage Export Templates > Install from File**;
it supports Windows x86_64 for the configuration recorded in the manifest.

The complete ready-to-ship gate runs the default bridge suite, native units,
package/source and launcher checks, actual Windows exports, and the four
isolated animation, protocol, release, and settings/setup workflows serially.
It writes per-check logs and a `.verification.json` only after every check passes:

```powershell
node scripts/verify-release.mjs --package "$mcpTemplateReleaseDirectory"
python scripts/package-windows-archive.py --package "$mcpTemplateReleaseDirectory" --output "$mcpTemplateReleaseDirectory.zip"
```

The ZIP builder requires that passing report and matching manifest, rejects
changed/unlisted files, verifies ZIP CRCs, inventory and entry hashes, and writes
a `.zip.sha256`. Outputs never overwrite existing archives. Upload the ZIP,
its checksum, the standalone TPZ/checksum, and the verification report as release
assets; do not commit binaries or local client configurations to Git.

## Coverage and limits

| Area | Available coverage | Limit |
| --- | --- | --- |
| MCP initialization and tool calls | Independent stdio clients; protocol versions `2024-11-05`, `2025-03-26`, `2025-06-18`, and `2025-11-25`; generic CLI export | Protocol tests do not demonstrate integration with every client application. |
| Discovery and reconnect | Session parsing, matching portable/source registries, explicit pins/cwd, replacement localhost endpoint, stale handshakes, safe/unsafe replay, bounded timeout/backoff | Replacement endpoints simulate editors; real unpinned portable registration is checked separately. |
| Cancellation and deadlines | Unit and actual SDK fixtures cover cancelled discovery/polling, a surviving concurrent caller, pending RPC cleanup, late replies, and deadlines covering backoff/RPC/acknowledgments | Cancellation/timeout stops waiting; an operation already sent may still complete. |
| Runtime and filesystem polling | Bridge waiter tests for stable deduplication IDs, completion, errors, arbitrary runtime call arguments, and acknowledgment failure | Completed results survive acknowledgment failure; no acknowledgment is sent for uncertain timeout/cancellation. Live runtime behavior is a separate workflow. |
| UTF-8 and request framing | Bridge response-fragment/reconnect tests plus native protocol requests split inside three/four-byte characters and an oversized multibyte request | The editor limit is 1 MiB per request in bytes. Native endpoint checks are gated live tests. |
| Unsaved editor buffers | Native buffer-conflict predicate and isolated release workflow with a real open unsaved script tab and scene | The release workflow is gated; the default suite does not execute it. |
| Validation, imports, reload, input, capture | Native validation tests, live runtime workflow, real-buffer checks, and mouse press/drag/release assertions for event masks/global positions | A skipped workflow establishes no execution evidence; mouse tests do not exhaust every window/input-routing combination. |
| Scene containment and owners | Native protocol workflow checks edited-scene containment, normalized/duplicate batch targets, subtree reparent/delete undo/redo, and save/reload packing | Runtime tree access has its separate run/root contract. These checks require the real rebuilt editor. |
| Scene patches | Native tests plus the live patch fixture cover global properties, ordered scripts, best-effort isolation, and internal front/back children during insert/move/undo | Public sibling positions exclude internal children. Selected cases do not exhaust every mutation ordering or custom setter. |
| Created resources | Native protocol checks pending lifetime, access/mutation/save, canonical paths, retargeting rejection, capacity release, and external-file conflicts | At most 128 pending resources are retained per editor MCP session; save before ending the session. |
| GDScript queries | Native protocol checks definitions, hover, document symbols, tokenized reference locations, and declaration exclusion | References are approximate lexical matches; workspace search is bounded to 300 files and 5,000 locations, with source-size/truncation limits. |
| Diagnostics | Native store isolation tests and two-run live assertions requiring each intentional runtime marker | This covers run isolation and marker delivery rather than every possible runtime error. |
| Packaging/setup | Isolated settings export, package hashes/source identity, actual SDK mutation/save, real restart/reconnect, moved bundle, and isolated release workflow | Only the exact tested payload is covered; existing release archives are not refreshed implicitly. |
| Shared client setup | All six schemas/scopes, preservation/conflict fixtures, CLI setup/removal, storage backups, doctor exit codes, real bridge diagnostics, and packaged launcher | Independent diagnostics and file registration do not establish tool visibility in every running agent application. |

## Test hygiene

Use disposable projects for live workflows, since they alter scenes/resources and start game runs. Do not terminate unrelated editors to replace locked binaries. Remove only owned temporary projects/scripts after their owned processes stop. Normal client configuration files must not be changed during testing.

`artifacts/` contains pre-existing untracked user data. Do not stage, overwrite, or remove it as part of MCP source changes. Existing archives must be treated separately from source and verification binaries when assessing a release.
