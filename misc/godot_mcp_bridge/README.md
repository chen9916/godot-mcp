# Godot MCP bridge

This package exposes a local MCP stdio server for compatible MCP clients. It forwards tool calls to the Godot editor's localhost JSON-RPC endpoint. Each client launches its own bridge process; all connections share editor state.

```text
MCP client -> stdio MCP (Node bridge) -> localhost JSON-RPC -> Godot editor
```

For supported release scope, executed checks, and remaining work, see [MCP status](../../MCP_STATUS.md).

## Requirements and build

- Node.js 18 or later.
- An editor built with the Godot MCP source changes. Rebuild the editor and bridge together when upgrading from previous identifiers; old portable archives retain their original code until repackaged.
- MCP enabled through `network/godot_mcp/enabled`, or through **Copy MCP Configuration** below.

Use the [bridge build commands](test/README.md#bridge-build-and-default-tests) from this directory. The same guide owns native build commands and optional live workflows.

## Shared setup for MCP clients

Use the same setup tool for **Codex, OpenCode, Hermes, Antigravity, Claude Code,
and Claude Desktop**. It registers this bridge in the selected client's native
configuration format; the MCP server and its tools are identical for every client.

A newly generated Windows bundle includes `godot-mcp.cmd` beside `Godot.exe` and
its bundled Node runtime. Open the game project in that editor, enable
`network/godot_mcp/enabled`, and run from the bundle directory:

```powershell
.\godot-mcp.cmd setup
.\godot-mcp.cmd setup --client antigravity --project-root "E:\Games\MyGame"
.\godot-mcp.cmd setup --client opencode,hermes --project-root "E:\Games\MyGame"
.\godot-mcp.cmd doctor --client antigravity
```

Interactive setup lists all targets and whether their configuration file was
found. Select only the clients you want. Non-interactive callers must use
`--client`; repeat it or pass a comma-separated list. `--client all` explicitly
selects every target. `claude` is an alias for `claude-code`; choose
`claude-desktop` separately for the Desktop app.

From a source checkout, build the bridge, then use `node dist/main.js setup`.
For installation with an existing Node runtime, `npm install -g <bridge-folder>`
or an `npm pack` tarball exposes the `godot-mcp` command after building. This
repository's private package is not published to npm; there is no public `npx`
package name to install yet. Older portable archives require regeneration to
include the new setup tool and launcher.

### Configuration paths and scope

`godot-mcp clients` prints the resolved paths. Default scope is user/global.
Use `--scope project --project-root <game-project>` for the supported project
formats, or `--config <actual-file>` to override a path for one client.

| Target | Default user configuration | Project configuration |
| --- | --- | --- |
| `codex` | `$CODEX_HOME/config.toml`, or `~/.codex/config.toml` | `.codex/config.toml` (client requires a trusted project) |
| `opencode` | `OPENCODE_CONFIG`, or `$XDG_CONFIG_HOME/opencode/opencode.json`, otherwise `~/.config/opencode/opencode.json` | `opencode.json` or existing `opencode.jsonc` |
| `hermes` | `$HERMES_HOME/config.yaml`; otherwise Windows `%LOCALAPPDATA%/hermes/config.yaml`, other platforms `~/.hermes/config.yaml` | Use user scope or an explicit file |
| `antigravity` | `~/.gemini/config/mcp_config.json` | `.agents/mcp_config.json` |
| `claude-code` | `~/.claude.json` | `.mcp.json` |
| `claude-desktop` | Windows `%APPDATA%/Claude/claude_desktop_config.json`; macOS `~/Library/Application Support/Claude/claude_desktop_config.json` | Use an explicit file if the app uses another location |

OpenCode V1 and V2 use different structures. Setup detects existing V2
`mcp.servers` configurations; use `--opencode-version 2` when creating a new V2
configuration. Existing JSONC files and comments are preserved. Runtime-only
`OPENCODE_CONFIG_CONTENT` needs an explicit `--config` choice. Alternate
`OPENCODE_CONFIG_DIR` and Claude Code `CLAUDE_CONFIG_DIR` profiles may require
`--config`; setup does not guess an undocumented active registry path. Claude
Desktop installations with a virtualized configuration path, and platforms
without a documented default path, should use the actual file from the app's
Developer settings with `--config`.

These formats follow the client documentation:
[Codex](https://developers.openai.com/codex/mcp),
[OpenCode](https://opencode.ai/docs/mcp-servers/) and
[V2](https://opencode.ai/v2/docs/mcp-servers/),
[Hermes](https://hermes-agent.nousresearch.com/docs/user-guide/features/mcp/),
[Antigravity](https://antigravity.google/docs/mcp), and
[Claude Code](https://code.claude.com/docs/en/mcp).

### Preview, update, remove, and diagnose

```text
godot-mcp setup --client codex --dry-run
godot-mcp setup --client codex --replace
godot-mcp config --client hermes
godot-mcp config
godot-mcp remove --client codex
godot-mcp doctor --client codex --json
```

`config` prints a client-specific snippet, or generic executable/arguments JSON
when no client is selected. It never writes settings. `--name` selects a server
key (default `godot`). Setup preserves unrelated settings and servers, refuses
malformed or ambiguous files, and creates a uniquely named backup before
atomically replacing an existing file. Matching registrations are idempotent;
disabled matching entries are enabled. A different launch requires `--replace`
or a different name. Removal also requires `--replace` if the entry no longer
points to this installation. Config symlinks need `--config` pointing to their
actual regular file. Inline/dotted Codex TOML layouts that cannot be edited
without disturbing other settings require manual conversion to table headers.

Diagnostics initialize the actual configured MCP command, list its tools, and
call the read-only `godot.session.info` tool. They report **bridge health**
separately from **editor connectivity**. Setup checks the bridge before writing;
an unavailable editor does not prevent registration. Exit codes are `0` for
success, `1` for configuration/startup failure, and `2` when the bridge works
but the editor check fails. `--no-check` permits offline registration, and
`--timeout-ms` bounds diagnostics. `--json` produces results for scripted callers.
Backups may contain existing client credentials; they are created with private
file permissions on platforms that support them.

Refresh or restart the selected client as directed in the result. A successful
file write and independent MCP check do not prove that an already-running agent
has reloaded its tools. Multiple processes can read/modify client configuration;
setup detects changed file contents before replacement, but applications do not
share a lock, so this remains an optimistic check.

Keep the bundle installed at its chosen location. If it moves, rerun setup with
`--replace` to update the absolute launch paths. Prefer `--project-root` when
multiple editors are open; setup requires a real `project.godot` for this pin.

## Manual configuration export

Open **Editor > Editor Settings > Network > Godot MCP** and use **Copy MCP Configuration** at the bottom of the settings window, below the settings pane. This enables the endpoint and copies JSON using the common `mcpServers` convention. Add the connection to your client's MCP settings. Clients using another format can use the exported `command` and `args`.

Portable layouts discover `bridge/dist/main.js` beside the editor executable and bundled Node at `runtime/node.exe` on Windows or `runtime/bin/node` on other platforms. Source builds in `bin/` discover `../misc/godot_mcp_bridge/dist/main.js` and fall back to Node on PATH. Advanced **Bridge Script** and **Node Executable** settings override discovery. Build the bridge first for a source checkout.

Copy the configuration again after moving the editor or bridge. Export runs asynchronously with a 30-second timeout and invokes Node directly. It copies connection details without editing client configuration files.

For command-line export, run from the built bridge directory:

```bash
node dist/main.js --print-config mcp --project-root "<game-project>"
node dist/main.js --print-config connection --project-root "<game-project>"
```

The `mcp` format contains a complete `mcpServers` JSON object. The `connection` format contains only `command` and `args`. Both use the running Node executable's absolute path. Choose stdio/local-process transport: the editor TCP port is an internal JSON-RPC endpoint, not an HTTP MCP URL.

## Project selection and manual launch

The game project contains `project.godot`; its session metadata is `.godot/godot_mcp/session.json`. Use `--project-root` when several editors are open, or `--session-file` to select an explicit metadata file. Without either pin, discovery walks upward from the working directory, then checks Godot's recent projects for the most recently modified session file.

```bash
node dist/main.js --project-root "<game-project>"
```

Optional launch arguments:

- `--project-root "<path>"`: selects the project's session file.
- `--session-file "<absolute-path>"`: overrides project discovery.
- `--timeout-ms 15000`: changes the RPC timeout; the default is 10000 milliseconds.

The MCP SDK handles initialization, version negotiation, discovery, and calls. Server stdout carries MCP messages; diagnostics go to stderr. Closing stdin releases the editor connection and exits the bridge. Independent stdio tests exercise four protocol versions; they do not launch agent applications. See [test coverage](test/README.md#coverage-and-limits).

## Tools and contracts

The public server identity is `godot-mcp`, and tool names use the `godot.*` prefix. The bridge verifies session UUID/PID metadata, reconnects with bounded backoff, and checks advertised capabilities before forwarding calls. Tool discovery can run without an editor; `godot.scene.patch_schema` returns the recursive patch schema offline. `godot.session.info` reports bridge state even while the editor is unavailable.

Use these references instead of a second tool/capability catalog:

- [Editor endpoint overview](../../editor/debugger/godot_mcp/README.md): settings and source entry points.
- [Automation tools v2](../../editor/debugger/godot_mcp/AUTOMATION_TOOLS_V2.md): tool contracts, capabilities, concurrency guards, runtime ownership, and result/error shapes.
- [Architecture](../../editor/debugger/godot_mcp/ARCHITECTURE.md): transport, sessions, resource state, and runtime modules.

The contracts describe intended behavior. Check [MCP status](../../MCP_STATUS.md) for known deviations before relying on mutations, save conflicts, or best-effort patches.

## Troubleshooting

- `SESSION_ERROR: Session file not found`: open the selected project in the rebuilt editor and enable `network/godot_mcp/enabled`.
- `SESSION_ERROR: Session metadata is expired`: restart or toggle the editor MCP setting.
- `TRANSPORT_ERROR: Could not connect`: confirm a running editor owns the UUID/port in the selected session file. The bridge retries with bounded backoff.
- `STALE_SESSION`: endpoint and metadata identify different editor sessions. Let discovery settle and refresh editor object state.
- `STALE_RUN`: start a new runtime and use its returned `run_id`; identities do not survive another run or editor session.
- Configuration export cannot find the bridge or Node: build the source bridge, check the portable layout, or set the advanced paths explicitly.

The editor endpoint binds to `127.0.0.1`.
