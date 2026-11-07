$s = 'C:\Users\shiva\AppData\Local\Temp\claude\dd6418d2605d4fa5\ae7d919f-98d3-467e-8ca0-aae07bf2d6fb\scratchpad'
$out = "$s\restart-wh-result.txt"
try {
  Restart-Service -Name Windhawk -Force -ErrorAction Stop
  Start-Sleep -Seconds 2
  "service=" + (Get-Service Windhawk).Status | Out-File $out
} catch { "ERROR: $($_.Exception.Message)" | Out-File $out }
