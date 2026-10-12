@echo off
setlocal
call "%~dp0godot-mcp.cmd" doctor %*
set "check_exit=%errorlevel%"
if "%check_exit%"=="2" echo The bridge works, but the editor is unavailable. Open your project in this bundle's Godot.exe and enable Network ^> Godot MCP.
if not defined GODOT_MCP_NO_PAUSE pause
exit /b %check_exit%
