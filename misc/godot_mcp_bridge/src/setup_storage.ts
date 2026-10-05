import { randomUUID } from "node:crypto";
import fs from "node:fs/promises";
import path from "node:path";

export interface ConfigSnapshot {
	text: string;
	exists: boolean;
	mode: number;
}

export async function read_config_file(p_filename: string): Promise<ConfigSnapshot> {
	try {
		const stat = await fs.lstat(p_filename);
		if (!stat.isFile() || stat.isSymbolicLink()) {
			throw new Error(`Configuration must be a regular file: ${p_filename}. Use --config to select the actual file.`);
		}
		return { text: await fs.readFile(p_filename, "utf8"), exists: true, mode: stat.mode & 0o777 };
	} catch (error: unknown) {
		if ((error as NodeJS.ErrnoException).code === "ENOENT") {
			return { text: "", exists: false, mode: 0o600 };
		}
		throw error;
	}
}

// Compare before replacement so a concurrent edit is not silently overwritten.
// This is an optimistic check; applications do not share a configuration lock.
export async function write_config_file(p_filename: string, p_original: ConfigSnapshot, p_text: string): Promise<{ backup?: string }> {
	const verify_original = async (): Promise<void> => {
		const current = await read_config_file(p_filename);
		if (current.exists !== p_original.exists || current.text !== p_original.text) {
			throw new Error(`Configuration changed during setup: ${p_filename}. Run setup again.`);
		}
	};
	await verify_original();
	await fs.mkdir(path.dirname(p_filename), { recursive: true });
	const suffix = randomUUID();
	const temporary = path.join(path.dirname(p_filename), `.godot-mcp-${suffix}.tmp`);
	let backup: string | undefined;
	try {
		await fs.writeFile(temporary, p_text, { flag: "wx", mode: p_original.mode });
		if (p_original.exists) {
			backup = `${p_filename}.godot-mcp-${suffix}.bak`;
			await fs.writeFile(backup, p_original.text, { flag: "wx", mode: 0o600 });
		}
		await verify_original();
		await fs.rename(temporary, p_filename);
		if ((await fs.readFile(p_filename, "utf8")) !== p_text) {
			throw new Error(`Configuration could not be verified after saving: ${p_filename}. Backup: ${backup ?? "new file"}.`);
		}
		return { backup };
	} finally {
		await fs.unlink(temporary).catch((error: NodeJS.ErrnoException) => {
			if (error.code !== "ENOENT") { throw error; }
		});
	}
}
