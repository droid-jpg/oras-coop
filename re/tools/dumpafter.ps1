# Waits, then dumps a move model several times (for capturing state while a key is held).
# Usage: powershell -File dumpafter.ps1 <mmHex> <delayMs> <prefix> <count> <gapMs>
param([string]$mm, [int]$delay, [string]$prefix, [int]$count = 3, [int]$gap = 150)
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
Start-Sleep -Milliseconds $delay
$start = [Convert]::ToUInt32($mm, 16)
for ($i = 0; $i -lt $count; $i++) {
  node E:\ORAS-RE\tools\mem.mjs dump ("0x{0:x}" -f $start) ("0x{0:x}" -f ($start + 0xAB0)) "$prefix$i.bin" | Out-Null
  Start-Sleep -Milliseconds $gap
}
"done $prefix"
