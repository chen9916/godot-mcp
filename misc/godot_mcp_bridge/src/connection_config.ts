import path from "node:path";

export interface ConnectionOptions {
	project_root?: string;
	bridge_script_path: string;
	session_file?: string;
}

export function bridge_command(p_options: ConnectionOptions): { command: string; args: string[] } {
	const args = [path.resolve(p_options.bridge_script_path)];
	if (p_options.project_root) {
		args.push("--project-root", path.resolve(p_options.project_root));
	}
	if (p_options.session_file) {
		args.push("--session-file", path.resolve(p_options.session_file));
	}
	return { command: process.execPath, args };
}
