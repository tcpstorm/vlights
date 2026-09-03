<#
.SYNOPSIS
  Builds LodLightRecolor.asi inside Docker and drops it in .\dist.

.PARAMETER Install
  Also copy the .asi (and the ini, if none is there yet) into the FiveM
  plugins folder.

.PARAMETER PluginsDir
  Override the FiveM plugins folder. Default: %LOCALAPPDATA%\FiveM\FiveM.app\plugins
#>
[CmdletBinding()]
param(
    [switch]$Install,
    [string]$PluginsDir = (Join-Path $env:LOCALAPPDATA 'FiveM\FiveM.app\plugins')
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dist = Join-Path $root 'dist'

docker build --target export --output "type=local,dest=$dist" $root
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "`nBuilt:" -ForegroundColor Green
Get-ChildItem $dist | Format-Table Name, Length, LastWriteTime -AutoSize

if ($Install) {
    if (-not (Test-Path $PluginsDir)) {
        New-Item -ItemType Directory -Force $PluginsDir | Out-Null
    }
    Copy-Item (Join-Path $dist 'LodLightRecolor.asi') $PluginsDir -Force
    $ini = Join-Path $PluginsDir 'lodlight_recolor.ini'
    if (-not (Test-Path $ini)) {
        Copy-Item (Join-Path $dist 'lodlight_recolor.ini') $ini
    }
    Write-Host "Installed to $PluginsDir" -ForegroundColor Green
}
