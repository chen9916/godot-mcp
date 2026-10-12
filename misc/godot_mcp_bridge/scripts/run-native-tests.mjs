import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const editor = process.argv[2];
if (!editor || !path.isAbsolute(editor) || !fs.statSync(editor).isFile()) {
	throw new Error("Usage: node scripts/run-native-tests.mjs <absolute-tests-editor-path>");
}
const repo = fileURLToPath(new URL("../../../", import.meta.url));
const temporaryBase = process.platform === "win32" && process.env.LOCALAPPDATA
	? path.join(process.env.LOCALAPPDATA, "Temp", "opencode") : os.tmpdir();
fs.mkdirSync(temporaryBase, { recursive: true });
const directory = fs.mkdtempSync(path.join(temporaryBase, "godot-mcp-native-"));
const bin = path.join(directory, "bin");
const executable = path.join(bin, process.platform === "win32" ? "Godot.exe" : "godot");
const profile = path.join(directory, "profile");
const temporary = path.join(directory, "temp");
const env = {
	...process.env,
	APPDATA: path.join(profile, "AppData", "Roaming"), LOCALAPPDATA: path.join(profile, "AppData", "Local"),
	USERPROFILE: profile, HOME: profile, TEMP: temporary, TMP: temporary, TMPDIR: temporary,
	XDG_CONFIG_HOME: path.join(profile, "config"), XDG_DATA_HOME: path.join(profile, "data"), XDG_CACHE_HOME: path.join(profile, "cache"),
};

try {
	fs.mkdirSync(bin);
	for (const name of ["APPDATA", "LOCALAPPDATA", "USERPROFILE", "TEMP", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"]) {
		fs.mkdirSync(env[name], { recursive: true });
	}
	fs.copyFileSync(editor, executable);
	fs.writeFileSync(path.join(bin, "_sc_"), "");
	fs.cpSync(path.join(repo, "tests", "data", "godot_mcp"), path.join(directory, "tests", "data", "godot_mcp"), { recursive: true });
	for (const [name, filter] of [["mcp", "*[GodotMCP]*"], ["undo", "*[UndoRedo]*"]]) {
		const stdoutPath = path.join(directory, `${name}.stdout.log`);
		const stderrPath = path.join(directory, `${name}.stderr.log`);
		const stdout = fs.openSync(stdoutPath, "w");
		const stderr = fs.openSync(stderrPath, "w");
		let result;
		try {
			result = spawnSync(executable, ["--headless", "--test", `--test-case=${filter}`], {
				cwd: directory, env, windowsHide: true, timeout: 180_000, stdio: ["ignore", stdout, stderr],
			});
		} finally {
			fs.closeSync(stdout);
			fs.closeSync(stderr);
		}
		const output = fs.readFileSync(stdoutPath, "utf8");
		process.stdout.write(output);
		const diagnostics = fs.readFileSync(stderrPath, "utf8");
		if (diagnostics) process.stderr.write(diagnostics.slice(0, 8192));
		if (result.error) throw result.error;
		if (result.status !== 0) throw new Error(`${name} native tests failed: ${result.status ?? result.signal}`);
		if (!output.includes("[doctest] Status: SUCCESS!") || !/\[doctest\] test cases:\s+[1-9]\d*\s+\|/.test(output)) {
			throw new Error(`${name} reported no passing native test cases. Supply an editor built with tests=yes.`);
		}
		if (/(?:^|\n)(?:SCRIPT ERROR|ERROR|WARNING):/.test(diagnostics)) {
			throw new Error(`${name} native tests emitted unexpected diagnostics.`);
		}
	}
} finally {
	// Remove only our copied binary; retain the small fixtures and evidence logs.
	fs.rmSync(executable, { force: true });
	console.log(`Native test logs retained at ${directory}`);
}
