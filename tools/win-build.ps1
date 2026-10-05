# SPDX-License-Identifier: MIT
# Helper for building esp32-zapret on Windows when the project lives in a
# path with non-ASCII characters (ESP-IDF's kconfgen cannot handle those).
# It mirrors the project into an ASCII work directory and runs idf.py there.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\win-build.ps1 -Action build
#   powershell -ExecutionPolicy Bypass -File tools\win-build.ps1 -Action flash-monitor -Port COM5
#
# Default paths assume this layout (see README):
#   D:\esp32-zapret\{esp-idf,python,tools,work,build}
# All paths can be overridden with -IdfPath/-ToolsPath/-PythonDir/-WorkDir/-BuildDir.

param(
    [ValidateSet("build", "menuconfig", "flash", "monitor", "flash-monitor", "clean")]
    [string]$Action = "build",
    [string]$WorkDir   = "D:\esp32-zapret\work",
    [string]$BuildDir  = "D:\esp32-zapret\build",
    [string]$IdfPath   = "D:\esp32-zapret\esp-idf",
    [string]$ToolsPath = "D:\esp32-zapret\tools",
    [string]$PythonDir = "D:\esp32-zapret\python",
    [string]$Port      = ""
)

$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $PSScriptRoot

Write-Host ">> mirroring $ProjectDir -> $WorkDir"
robocopy $ProjectDir $WorkDir /E /XD build dist /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) {
    throw "robocopy failed with exit code $LASTEXITCODE"
}

$portArg = if ($Port) { "-p $Port " } else { "" }
$idfCmd = switch ($Action) {
    "clean"         { "idf.py -B $BuildDir fullclean" }
    "flash"         { "idf.py -B $BuildDir ${portArg}flash" }
    "monitor"       { "idf.py -B $BuildDir ${portArg}monitor" }
    "flash-monitor" { "idf.py -B $BuildDir ${portArg}flash monitor" }
    default         { "idf.py -B $BuildDir $Action" }
}

$inner = "set IDF_TOOLS_PATH=$ToolsPath&& "
if ($PythonDir) {
    $inner += "set PATH=$PythonDir;%PATH%&& "
}
$inner += "cd /d $IdfPath&& call export.bat >nul&& cd /d $WorkDir&& $idfCmd"
Write-Host ">> $idfCmd"
cmd /c $inner
exit $LASTEXITCODE
