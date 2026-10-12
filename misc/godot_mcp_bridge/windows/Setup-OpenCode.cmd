@echo off
call "%~dp0Setup.cmd" --client opencode %*
exit /b %errorlevel%
