<#
.SYNOPSIS
  core (Arduino 非依存のロジック) のホストテストを MSVC でビルドして実行する。

.DESCRIPTION
  vswhere で VC ツール入りの Visual Studio を探し、vcvars64.bat を通して cl を呼ぶ。
  firmware/ToolCheck/src/core/*.cpp と tests/*.cpp を 1 本の exe にまとめる。
  警告はエラー扱い (/W4 /WX)。ESP32 側の gcc でも通すので、MSVC 固有の書き方に寄せない。

.EXAMPLE
  pwsh -NoProfile -File tests/run.ps1
  pwsh -NoProfile -File tests/run.ps1 -Filter classifier
#>
[CmdletBinding()]
param(
    [string]$Filter = ''
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    Write-Error 'vswhere.exe が見つかりません (Visual Studio が入っていない)'
    exit 1
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
    Write-Error 'VC ツール (C++ によるデスクトップ開発) 入りの Visual Studio が見つかりません'
    exit 1
}
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$coreDir = Join-Path $root 'firmware\ToolCheck\src\core'
$sources = @(Get-ChildItem -LiteralPath $coreDir -Filter '*.cpp' -ErrorAction SilentlyContinue) +
           @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.cpp')
$include = Join-Path $root 'firmware\ToolCheck\src'
$exe = Join-Path $build 'core_tests.exe'
$fileArgs = ($sources | ForEach-Object { '"' + $_.FullName + '"' }) -join ' '

$cl = "cl /nologo /std:c++17 /utf-8 /EHsc /W4 /WX /permissive- /Zc:__cplusplus " +
      "/I`"$include`" /Fo`"$build\\`" /Fe`"$exe`" $fileArgs"
cmd /c "`"$vcvars`" >nul && $cl"
if ($LASTEXITCODE -ne 0) {
    Write-Output "ビルドに失敗しました (exit=$LASTEXITCODE)"
    exit $LASTEXITCODE
}

if ($Filter) { & $exe $Filter } else { & $exe }
exit $LASTEXITCODE
