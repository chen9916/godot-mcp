# Godot MCP: Windows quick start

This custom Godot editor exposes **83 MCP tools** to compatible coding agents,
including scene editing, scripts, runtime debugging, and animation authoring.
The Windows x64 package includes Node.js, the bridge, and matching Windows
debug/release export templates. No compiler, npm installation, API key, or admin
access is needed to use the package. It is based on **Godot 4.8-dev**, not an
official stable Godot release; back up projects before opening them.

## Set up in five steps

1. Download the Windows ZIP from the repository's [Releases](https://github.com/chen9916/godot-mcp/releases)
   page. Extract it into a permanent, writable folder; do not launch inside the
   ZIP or install under a protected folder such as `Program Files`.
2. Run **Godot.exe** from that folder and open your game project. An ordinary
   Godot download does not contain these MCP tools.
3. Open **Editor > Editor Settings > Network > Godot MCP** and enable MCP.
4. Double-click **Setup-OpenCode.cmd** for OpenCode, or **Setup.cmd** to choose
   Codex, OpenCode, Hermes, Antigravity, Claude Code, or Claude Desktop. Setup
   uses your user/global configuration, preserves unrelated settings, and backs
   up existing files. New OpenCode configurations use V2; existing V1/V2 formats
   are preserved.
5. Refresh/reopen the selected agent. Run **Check-Connection.cmd** to confirm
   that both the bridge and editor are connected. In OpenCode, also check
   `opencode mcp list` or `/mcps`.

Try asking your agent:

> Inspect the active scene and AnimationTree without changing anything. Validate
> the graph, then preview a small edit with dry_run before applying it.

The portable editor stores its own settings beside the executable in
`editor_data`. Keep the editor running while the agent uses its tools. The
localhost JSON-RPC port is internal; do not enter it as an HTTP MCP URL.
`Godot.console.exe` starts the same editor with console output for diagnostics.

## Updating or choosing a project

Extract upgrades into a **new folder**; do not mix old and new editor, bridge,
and template files. Save your work and close the old editor before opening the
new one. From a terminal in the new bundle:

```powershell
.\godot-mcp.cmd setup --client opencode --replace
.\godot-mcp.cmd doctor --client opencode
```

If several Godot projects/editors are open, pin the intended project:

```powershell
.\godot-mcp.cmd setup --client opencode --project-root "C:\Games\MyGame" --replace
```

`--replace` explicitly updates an existing Godot registration to this bundle.
Use `--dry-run` to preview configuration changes without writing. Rerun setup
after moving the bundle, because client launch paths are absolute. For an old
OpenCode installation without an existing configuration, add
`--opencode-version 1`.

If upgrading from the old OpenCode-only bridge, disable its `godot-opencode`
registration in your client after confirming the new `godot` connection. Setup
does not delete a differently named registration or unrelated configuration.

## If something is not connected

| Symptom | What to check |
| --- | --- |
| Bridge OK, editor unavailable (exit 2) | Open the intended project in this package's editor and enable MCP. Registration can succeed while the editor is offline. |
| Old tool list or scripted graph edits still rejected | Close the old editor, open the new one, and refresh the agent's MCP connection. |
| Configuration already points elsewhere | Preview with `--dry-run`, then use `--replace` if this is the intended upgrade. |
| Setup failed (exit 1) | Read the displayed error; see `bridge/README.md` for alternate config paths and client-specific scope. |

For removal, use `godot-mcp.cmd remove --client opencode`; this removes only this
installation's named registration. Alternative clients and flags are listed by
`godot-mcp.cmd clients` and `godot-mcp.cmd setup --help`.

## Scope and safety

- Only Windows x64 binaries/templates are included. This is not a certification
  of every renderer, graphics driver, game project, or extension. The binaries
  are unsigned; Windows may show a warning. Verify the published SHA-256 before
  deciding whether to run them.
- Agents can modify projects. Review changes and keep backups/version control.
  Scene/animation patches have guards, local resource copies, dry runs, and undo;
  saving is explicit. A timeout does not prove an already-dispatched edit was
  rolled back, so inspect before retrying.
- Ordinary non-`@tool` AnimationTree controller scripts are preserved during
  graph editing. Editor-executing tree scripts and custom scripted AnimationNode
  resources remain restricted.
- MCP runtime inspection is for owned debug runs. Release exports do not expose
  the debug-only MCP runtime integration.

The bundle's `RELEASE_MANIFEST.json` records the source commit, versions, and
file hashes. The release includes checksums and a verification report. Developers
can reproduce the checks using
[the build/test guide](https://github.com/chen9916/godot-mcp/blob/master/misc/godot_mcp_bridge/test/README.md).
