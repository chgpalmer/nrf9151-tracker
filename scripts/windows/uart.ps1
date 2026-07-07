param([string]$Port = "COM8", [int]$Baud = 115200)
$p = New-Object System.IO.Ports.SerialPort($Port, $Baud, 'None', 8, 'One')
$p.DtrEnable = $true
$p.RtsEnable = $true
$p.ReadTimeout = 500
try { $p.Open() } catch { Write-Host "ERROR opening $Port : $($_.Exception.Message)"; exit 1 }
Write-Host "== $Port @ $Baud  (Ctrl-C to quit) =="
while ($true) {
  try { $s = $p.ReadExisting(); if ($s) { [Console]::Write($s) } } catch {}
  Start-Sleep -Milliseconds 50
}
