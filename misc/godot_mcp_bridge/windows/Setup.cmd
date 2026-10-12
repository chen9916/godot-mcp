@echo off
setlocal
echo Godot MCP setup
echo Open your project in this bundle's Godot.exe and enable Network ^> Godot MCP in Editor Settings.
echo Setup preserves unrelated client settings and backs up existing configuration files.
call "%~dp0godot-mcp.cmd" setup %*
set "setup_exit=%errorlevel%"
if "%setup_exit%"=="2" echo Registration succeeded, but Godot is not connected yet. Open your project, enable MCP, and run Check-Connection.cmd.
if "%setup_exit%"=="1" echo Setup failed. Review the message above; use --dry-run to preview or --replace to explicitly update an old registration.
if not defined GODOT_MCP_NO_PAUSE pause
exit /b %setup_exit%
