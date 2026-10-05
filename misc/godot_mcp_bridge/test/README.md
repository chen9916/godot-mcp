# Godot MCP build and test guide

These commands describe available checks. See [MCP status](../../../MCP_STATUS.md) for recorded verification, supported release scope, and remaining work.

## Bridge build and default tests

From `misc/godot_mcp_bridge`, with Node.js 18 or later:

```powershell
npm.cmd ci
npm.cmd run typecheck
npm.cmd test
npm.cmd run build
```

On platforms without `npm.cmd`, use `npm`. `npm test` compiles `.test-dist` and runs unit, localhost transport, independent stdio, and configuration-export tests. Its runner selects outputs corresponding to current test sources, excluding stale outputs from deleted or renamed tests. It does not require Godot unless optional workflow environment variables are set. The four live workflows below are skipped by default; a successful default run therefore leaves those workflows unverified.

The default suite also checks shared client setup for Codex, OpenCode V1/V2,
Hermes, Antigravity, Claude Code, and Claude Desktop using isolated files. It
exercises comment/settings preservation, malformed and ambiguous documents,
environment and scope paths, disabled registrations, repeated setup, removal,
backups, concurrent edits, and CLI subprocesses. Doctor tests initialize the real
bridge against offline and fixture editor endpoints and verify bounded process
termination. They do not install into the user's agent profiles.

## Native editor build and tests

Godot-side tests in `tests/editor/godot_mcp/` are included when building with `tests=yes`. The following Windows command uses the sibling SCons checkout provided in this workspace. Run from the engine root with the required Godot Windows build dependencies installed:

```powershell
$env:PYTHONPATH = (Resolve-Path "../scons").Path
python -m SCons platform=windows target=editor tests=yes extra_suffix=mcp_latest_verify werror=yes debug_symbols=no accesskit=no angle=no -j12
./bin/godot.windows.editor.x86_64.mcp_latest_verify.console.exe --headless --test --test-case="*[GodotMCP]*"
```

The suffix creates a separate verification binary so an unrelated running editor need not be replaced. Other platforms require their corresponding build options and binary name.

## Live runtime workflow

Use a disposable project containing `project.godot`, open it in the rebuilt editor, enable `network/godot_mcp/enabled`, and close game runs belonging to that test project. From the bridge directory:

```powershell
npm.cmd run build:test
$env:GODOT_MCP_INTEGRATION = "1"
$env:GODOT_MCP_INTEGRATION_PROJECT = "C:\absolute\path\to\disposable-test-project"
node --test .test-dist/test/editor_integration.test.js
```

The test validates disk and supplied invalid GDScript, creates/scans/imports an SVG, reloads a changed disk script, reads partial settings, starts two game runs, inspects and writes runtime properties, calls a method, checks paused physics/process stepping and held input, resumes, captures a viewport, and checks run-filtered diagnostics. It refuses to overwrite an existing fixture directory. An `after` hook stops its owned run, removes its fixture and captured PNG, scans the filesystem, and closes the bridge connection.

## Live scene-patch workflow

This workflow uses the same disposable-project setting but a separate flag and test process. Run it separately from the runtime workflow because both change editor scene/run state:

```powershell
npm.cmd run build:test
$env:GODOT_MCP_SCENE_PATCH_INTEGRATION = "1"
$env:GODOT_MCP_INTEGRATION_PROJECT = "C:\absolute\path\to\disposable-test-project"
node --test .test-dist/test/scene_patch_integration.test.js
```

It exercises root rename dry runs/commits, subtree move/save, JSON integer positions and packed arrays, sibling collision rejection, and returned paths after property-based renames. It creates a unique fixture directory and removes it afterward. Native scene-patch tests separately cover undo/redo, snapshots, packing, and SceneTree ownership.

## Isolated editor setup workflow

On Windows, this workflow makes a temporary self-contained bundle and project using the selected rebuilt executable, current Node executable, and bridge build/dependencies. It launches its own headless editor and inspects Editor Settings controls through an editor plugin:

```powershell
npm.cmd run build
npm.cmd run build:test
$env:GODOT_MCP_SETUP_EDITOR = "C:\absolute\path\to\rebuilt\Godot.exe"
node --test .test-dist/test/editor_setup_integration.test.js
```

It checks the Godot MCP category/search with localization on/off, config-button export, enabled session, repeat export, missing-bridge recovery, moved bundle paths, and source-checkout discovery with Node on PATH. An independent SDK client launches the exported connection and checks tool discovery and the live session handshake. It does not launch a coding agent or install configuration into an agent application. The temporary files are removed after its editor exits.

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
Remove-Item Env:GODOT_MCP_INTEGRATION, Env:GODOT_MCP_SCENE_PATCH_INTEGRATION, Env:GODOT_MCP_INTEGRATION_PROJECT, Env:GODOT_MCP_SETUP_EDITOR, Env:GODOT_MCP_RELEASE_EDITOR, Env:GODOT_MCP_RELEASE_BUNDLE -ErrorAction SilentlyContinue
```

## Windows packaging and package verification

Commit the final source and build its editor before packaging. The packager
requires a clean checkout and a new output directory under `artifacts/`; it
refuses to overwrite an existing package. Supply the complete license from the
selected Node distribution. Run from the bridge directory:

```powershell
npm.cmd run package:windows -- --editor "C:\absolute\path\to\rebuilt\Godot.exe" --output "C:\absolute\path\to\checkout\artifacts\new-release" --node-license "C:\absolute\path\to\Node\LICENSE"
node scripts/verify-package.mjs --package "C:\absolute\path\to\checkout\artifacts\new-release" --check-source
$env:GODOT_MCP_RELEASE_BUNDLE = "C:\absolute\path\to\checkout\artifacts\new-release"
node --test .test-dist/test/release_editor_integration.test.js
```

The payload contains the editor, `godot-mcp.cmd` setup launcher, bridge build, bundled Node, lockfile-installed
production dependencies, and notices. Its manifest records the source commit,
source and payload hashes, and executable versions. `--check-source` verifies
that the clean checkout still matches that manifest. The smoke verifier checks
the actual launcher with quoted paths, setup/doctor against a disposable client
file, an independent SDK client, scene mutation/save, saved state across an editor
restart, reconnecting the same bridge, and exporting/connecting after moving a
temporary copy. Packaging currently supports Windows x86_64.

## Coverage and limits

| Area | Available coverage | Limit |
| --- | --- | --- |
| MCP initialization and tool calls | Independent stdio clients; protocol versions `2024-11-05`, `2025-03-26`, `2025-06-18`, and `2025-11-25`; generic CLI export | Protocol tests do not demonstrate integration with every client application. |
| Discovery and reconnect | Session parsing/discovery tests, replacement localhost endpoint, stale handshakes, safe/unsafe replay, bounded timeout/backoff | Replacement endpoints simulate editors; actual editor setup is a separate workflow. |
| Runtime and filesystem polling | Bridge waiter tests for deduplication, completion, errors, and timeouts | Mocked transport coverage does not replace the live runtime workflow. |
| Unsaved editor buffers | Native buffer-conflict predicate and isolated release workflow with a real open unsaved script tab and scene | The release workflow is gated; the default suite does not execute it. |
| Validation, imports, reload, input, capture | Native validation tests, live runtime workflow, and isolated real-buffer checks | A skipped workflow establishes no execution evidence. |
| Scene patches | Native scene-patch tests plus the separate live patch fixture | Covers global properties, ordered script assignments, best-effort isolation, apply/undo/redo, persistence, and reload. Selected cases do not exhaust every mutation ordering or custom setter. |
| Diagnostics | Native store isolation tests and two-run live assertions requiring each intentional runtime marker | This covers run isolation and marker delivery rather than every possible runtime error. |
| Packaging/setup | Isolated settings export, package hashes/source identity, actual SDK mutation/save, real restart/reconnect, moved bundle, and isolated release workflow | Only the exact tested payload is covered; existing release archives are not refreshed implicitly. |
| Shared client setup | All six target schemas/scopes, preservation and conflict fixtures, CLI setup/removal, storage backups, real bridge diagnostics, and packaged launcher | Independent diagnostics and file registration do not establish tool visibility in every running agent application. |

## Test hygiene

Use disposable projects for live workflows, since they alter scenes/resources and start game runs. Do not terminate unrelated editors to replace locked binaries. Remove only owned temporary projects/scripts after their owned processes stop. Normal client configuration files must not be changed during testing.

`artifacts/` contains pre-existing untracked user data. Do not stage, overwrite, or remove it as part of MCP source changes. Existing archives must be treated separately from source and verification binaries when assessing a release.
