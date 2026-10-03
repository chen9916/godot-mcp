import { readdirSync } from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL("../", import.meta.url));
// TypeScript retains outputs for removed source files. Only run current tests.
const tests = readdirSync(path.join(root, "test"))
	.filter((name) => name.endsWith(".test.ts"))
	.sort()
	.map((name) => path.join(root, ".test-dist", "test", name.replace(/\.ts$/, ".js")));
if (!tests.length) throw new Error("No current test sources found.");
const result = spawnSync(process.execPath, ["--test", ...tests], { cwd: root, stdio: "inherit", windowsHide: true });
if (result.error) throw result.error;
process.exit(result.status ?? 1);
