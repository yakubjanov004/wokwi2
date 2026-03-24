$ErrorActionPreference = "Stop"

Set-Location "$PSScriptRoot\.."

$pioCoreDir = Join-Path (Get-Location) ".platformio-home"
if (-not (Test-Path $pioCoreDir)) {
  New-Item -ItemType Directory -Path $pioCoreDir | Out-Null
}
$env:PLATFORMIO_CORE_DIR = (Resolve-Path $pioCoreDir).Path

$pioExe = Join-Path (Get-Location) "venv\Scripts\platformio.exe"
if (-not (Test-Path $pioExe)) {
  throw "PlatformIO executable not found at '$pioExe'. Activate/create the venv first."
}
$pio = (Resolve-Path $pioExe).Path

function Get-DefinedPioEnvNames() {
  $jsonText = & $pio project config --json-output
  if ($LASTEXITCODE -ne 0) {
    throw "Unable to read PlatformIO project config"
  }

  $config = $jsonText | ConvertFrom-Json
  $names = @()
  foreach ($entry in $config) {
    $sectionName = [string]$entry[0]
    if ($sectionName.StartsWith("env:")) {
      $names += $sectionName.Substring(4)
    }
  }
  return $names
}

function Invoke-PioBuild([string]$EnvName) {
  Write-Host "[BUILD] $EnvName"
  & $pio run -e $EnvName
  if ($LASTEXITCODE -ne 0) {
    throw "PlatformIO build failed for $EnvName"
  }
}

function Invoke-PioBuildFs([string]$EnvName) {
  Write-Host "[BUILD-FS] $EnvName"
  & $pio run -e $EnvName -t buildfs
  if ($LASTEXITCODE -ne 0) {
    throw "PlatformIO filesystem build failed for $EnvName"
  }
}

Invoke-PioBuild "esp32dev"
Invoke-PioBuildFs "esp32dev"

$definedEnvNames = Get-DefinedPioEnvNames

for ($i = 1; $i -le 9; $i++) {
  $envName = "table_node$i"
  if ($definedEnvNames -contains $envName) {
    Invoke-PioBuild $envName
  } else {
    Write-Host "[SKIP] $envName (environment is not defined in platformio.ini)"
  }
}

Write-Host "[OK] All firmware builds completed."
