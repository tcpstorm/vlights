<#
.SYNOPSIS
  Turn a screen recording into a README-sized GIF using ffmpeg in Docker.
  Nothing is installed on the host.

.EXAMPLE
  .\tools\mp4togif.ps1 .\docs\media\demo.mp4
  .\tools\mp4togif.ps1 .\demo.mp4 -Out .\docs\media\demo.gif -Width 800 -Fps 15 -Start 2 -Duration 8

.NOTES
  Two-pass palette conversion (palettegen + paletteuse) keeps colours clean,
  which matters here since the colours are the point. Keep the result under
  ~10 MB or GitHub will not animate it inline: shorten -Duration, lower
  -Fps or -Width.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$In,
    [string]$Out,
    [int]$Width = 800,
    [int]$Fps = 15,
    [double]$Start = 0,
    [double]$Duration = 0,
    [int]$Colors = 128
)

$ErrorActionPreference = 'Stop'
$inPath = (Resolve-Path $In).Path
if (-not $Out) { $Out = [IO.Path]::ChangeExtension($inPath, '.gif') }
$outDir = Split-Path -Parent ([IO.Path]::GetFullPath($Out))
$outName = Split-Path -Leaf $Out
$inDir = Split-Path -Parent $inPath
$inName = Split-Path -Leaf $inPath

$trim = @()
if ($Start -gt 0) { $trim += @('-ss', "$Start") }
if ($Duration -gt 0) { $trim += @('-t', "$Duration") }

$filter = "fps=$Fps,scale=${Width}:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=$Colors[p];[b][p]paletteuse=dither=bayer:bayer_scale=3"

docker run --rm `
    -v "${inDir}:/in:ro" `
    -v "${outDir}:/out" `
    jrottenberg/ffmpeg:6-alpine `
    -y @trim -i "/in/$inName" -vf $filter -loop 0 "/out/$outName"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$size = (Get-Item (Join-Path $outDir $outName)).Length / 1MB
Write-Host ("{0}  ({1:N1} MB)" -f (Join-Path $outDir $outName), $size) -ForegroundColor Green
if ($size -gt 10) { Write-Warning "Over 10 MB: GitHub may not animate it inline. Try -Width 640, -Fps 12, or a shorter -Duration." }
