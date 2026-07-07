# Bind + attach the nRF9151-DK J-Link into WSL via usbipd-win.
# Invoked from WSL by `make windows-usb-passthrough` (through powershell.exe).
# Fully self-contained so a fresh checkout needs nothing but `make`:
#   - installs usbipd-win via winget if it's missing
#   - locates usbipd even when it isn't on PATH yet (freshly installed service)
#   - self-elevates (UAC) to run `bind`, which requires admin; `attach` does not
# The SEGGER J-Link enumerates as VID 1366.

$ErrorActionPreference = 'Stop'
function Fail($msg) { Write-Host "ERROR: $msg"; exit 1 }

# --- locate or install usbipd -------------------------------------------------
function Get-UsbipdPath {
  $c = Get-Command usbipd -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  foreach ($p in @(
      "$env:ProgramFiles\usbipd-win\usbipd.exe",
      "${env:ProgramFiles(x86)}\usbipd-win\usbipd.exe")) {
    if (Test-Path $p) { return $p }
  }
  return $null
}

$usbipd = Get-UsbipdPath
if (-not $usbipd) {
  Write-Host "== usbipd not found -- installing via winget =="
  winget install --id dorssel.usbipd-win -e --accept-source-agreements --accept-package-agreements
  $usbipd = Get-UsbipdPath
  if (-not $usbipd) {
    Fail "usbipd still not found after install. Open a NEW terminal and re-run 'make windows-usb-passthrough'."
  }
}
Write-Host "usbipd: $usbipd"

# --- find the SEGGER J-Link (VID 1366) ---------------------------------------
Write-Host "== usbipd list =="
& $usbipd list
$line = (& $usbipd list) | Select-String -Pattern '1366:' | Select-Object -First 1
if (-not $line) { Fail "No SEGGER J-Link (VID 1366) found. Is the DK plugged in?" }
$busid = ($line.ToString().Trim() -split '\s+')[0]
Write-Host "Found J-Link at busid $busid"

# --- bind (needs admin) -------------------------------------------------------
# Skip only if the STATE column says "Shared" (NOT "Not shared" -- note the
# substring trap). This DK also carries a 'CsDeviceControl' USB filter, so bind
# needs --force. Self-elevate a one-shot bind via UAC.
$state = $line.ToString().Trim()
$isShared = ($state -notmatch '(?i)not shared') -and ($state -match '(?i)shared')
if (-not $isShared) {
  Write-Host "== usbipd bind --force --busid $busid (elevating; approve the UAC prompt) =="
  $p = Start-Process -FilePath $usbipd -ArgumentList @('bind','--force','--busid',$busid) `
        -Verb RunAs -Wait -PassThru
  if ($p.ExitCode -ne 0) {
    Fail "bind failed ($($p.ExitCode)). Run once in an ADMIN PowerShell: usbipd bind --force --busid $busid"
  }
} else {
  Write-Host "busid $busid already shared -- skipping bind"
}

# --- attach into WSL (no admin) ----------------------------------------------
Write-Host "== usbipd attach --wsl --busid $busid =="
$a = Start-Process -FilePath $usbipd -ArgumentList @('attach','--wsl','--busid',$busid) `
      -Wait -PassThru -NoNewWindow
if ($a.ExitCode -ne 0) { Fail "attach failed ($($a.ExitCode)). Try: usbipd attach --wsl --busid $busid" }

Write-Host "Attached. In WSL check: ls /dev/ttyACM*  and  lsusb | grep -i segger"
