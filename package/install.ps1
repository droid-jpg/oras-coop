# oras-mmo installer for Azahar (Windows). Run through "Install oras-mmo.bat".
#   -AzaharDir <path>  Azahar user folder (default %APPDATA%\Azahar, or the portable "user" folder)
#   -Uninstall         remove the plugin (keeps oras-mmo.txt and Azahar's settings)
param(
    [string]$AzaharDir,
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'

$TitleId  = '000400000011C500'   # Pokemon Alpha Sapphire
$Here     = Split-Path -Parent $MyInvocation.MyCommand.Path
$Utf8     = New-Object System.Text.UTF8Encoding($false)

function Ask([string]$prompt, [string]$default) {
    $v = Read-Host "$prompt [$default]"
    if ([string]::IsNullOrWhiteSpace($v)) { return $default }
    return $v.Trim()
}

# The frames carry plain ASCII: name[16] and room[12], NUL-padded.
function Clean([string]$s, [int]$max) {
    $s = ($s -replace '[^\x20-\x7e]', '').Trim()
    if ($s.Length -gt $max) { $s = $s.Substring(0, $max) }
    return $s
}

# --- Find Azahar's user folder ----------------------------------------------------------------
if (-not $AzaharDir) {
    $AzaharDir = Join-Path $env:APPDATA 'Azahar'
    if (-not (Test-Path $AzaharDir)) {
        Write-Host "Azahar's folder wasn't found at $AzaharDir."
        Write-Host "Start Azahar once (so it creates it), or paste the path to a portable Azahar's 'user' folder."
        $AzaharDir = Read-Host 'Azahar user folder'
    }
}
$AzaharDir = $AzaharDir.Trim('"', ' ')
if (-not (Test-Path $AzaharDir)) { New-Item -ItemType Directory -Path $AzaharDir | Out-Null }

$PluginDir = Join-Path $AzaharDir "sdmc\luma\plugins\$TitleId"
$Plugin    = Join-Path $PluginDir 'oras-mmo.3gx'
$ConfigTxt = Join-Path $PluginDir 'oras-mmo.txt'
$QtConfig  = Join-Path $AzaharDir 'config\qt-config.ini'
$OldPlugin = Join-Path $PluginDir 'oras-coop.3gx'   # the mod's name before it became oras-mmo

# Azahar rewrites qt-config.ini on exit, which would undo the plugin-loader switch.
while (Get-Process -Name 'azahar*' -ErrorAction SilentlyContinue) {
    Read-Host 'Azahar is running. Close it, then press Enter'
}

if ($Uninstall) {
    if (Test-Path $OldPlugin) { Remove-Item $OldPlugin; Write-Host "Removed $OldPlugin" }
    if (Test-Path $Plugin) { Remove-Item $Plugin; Write-Host "Removed $Plugin" }
    else { Write-Host 'oras-mmo is not installed.' }
    exit 0
}

# --- Copy the plugin --------------------------------------------------------------------------
$Src = Join-Path $Here 'oras-mmo.3gx'
if (-not (Test-Path $Src)) { throw "oras-mmo.3gx is missing next to the installer ($Here)." }
New-Item -ItemType Directory -Force -Path $PluginDir | Out-Null
# Replace an install from before the rename, keeping its settings and model cache.
if (Test-Path $OldPlugin) { Remove-Item $OldPlugin; Write-Host "Removed the old $OldPlugin" }
foreach ($n in '.txt', '-models.bin') {
    $old = Join-Path $PluginDir "oras-coop$n"
    $new = Join-Path $PluginDir "oras-mmo$n"
    if ((Test-Path $old) -and -not (Test-Path $new)) { Move-Item $old $new }
}
Copy-Item $Src $Plugin -Force
Write-Host "Installed plugin to $Plugin"

# --- Player settings (oras-mmo.txt, read by the plugin) --------------------------------------
$writeConfig = $true
if (Test-Path $ConfigTxt) {
    $writeConfig = (Ask 'Settings already exist. Change them? (y/n)' 'n') -match '^[yY]'
}
if ($writeConfig) {
    Write-Host ''
    Write-Host 'Settings (you can change them later in the game: press SELECT for the plugin menu).'
    $name = Clean (Ask 'Your name' (Clean $env:USERNAME 15)) 15
    if (-not $name) { $name = 'Trainer' }
    $server = Ask 'Server address (whoever hosts runs "Start server.bat" and gives you their IP)' '127.0.0.1'
    # The plugin only takes an IPv4 address, so resolve a hostname here.
    $ip = $null
    if (-not [System.Net.IPAddress]::TryParse($server, [ref]$ip)) {
        try {
            $ip = [System.Net.Dns]::GetHostAddresses($server) |
                  Where-Object { $_.AddressFamily -eq 'InterNetwork' } | Select-Object -First 1
        } catch { $ip = $null }
        if (-not $ip) { throw "Couldn't find an IPv4 address for '$server'." }
        Write-Host "  $server -> $ip (if the host's IP changes, run the installer again)"
        $server = $ip.ToString()
    }
    $port  = Ask 'Server port' '7777'
    $room  = (Clean (Ask 'Room code (everyone in the same room sees each other)' 'HOENN') 12).ToUpper()
    if (-not $room) { $room = 'HOENN' }
    $models = (Ask 'Show friends as real trainers instead of name tags? (y/n)' 'y') -match '^[yY]'

    $lines = @("server=$server", "port=$port", "room=$room", "name=$name",
               "puppets=$(if ($models) { '1' } else { '0' })")
    if (Test-Path $ConfigTxt) {
        # Keep settings the installer doesn't ask about (greeting, scale, overlay).
        $keep = Get-Content $ConfigTxt | Where-Object { $_ -match '^(greeting|scale|overlay)=' }
        $lines += $keep
    }
    [System.IO.File]::WriteAllText($ConfigTxt, (($lines -join "`n") + "`n"), $Utf8)
    Write-Host "Saved settings to $ConfigTxt"
}

# --- Turn on Azahar's 3GX plugin loader -------------------------------------------------------
# qt-config.ini is a QSettings file: each setting has "key\default=true|false" plus "key=value".
function Set-IniKeys([string[]]$lines, [string]$section, [System.Collections.IDictionary]$values) {
    $out = New-Object System.Collections.Generic.List[string]
    $inSection = $false; $sawSection = $false; $done = @{}
    foreach ($line in $lines) {
        if ($line -match '^\[(.+)\]\s*$') {
            if ($inSection) { foreach ($k in $values.Keys) { if (-not $done[$k]) { $out.Add("$k=$($values[$k])") } } }
            $inSection = $Matches[1] -eq $section
            if ($inSection) { $sawSection = $true }
        } elseif ($inSection -and $line -match '^([^=]+)=') {
            $k = $Matches[1]
            if ($values.Contains($k)) { $out.Add("$k=$($values[$k])"); $done[$k] = $true; continue }
        }
        $out.Add($line)
    }
    if ($inSection) { foreach ($k in $values.Keys) { if (-not $done[$k]) { $out.Add("$k=$($values[$k])") } } }
    if (-not $sawSection) {
        $out.Add("[$section]")
        foreach ($k in $values.Keys) { $out.Add("$k=$($values[$k])") }
    }
    return ,$out.ToArray()
}

$lines = @()
if (Test-Path $QtConfig) {
    Copy-Item $QtConfig "$QtConfig.bak-oras-mmo" -Force
    $lines = [System.IO.File]::ReadAllLines($QtConfig)
} else {
    New-Item -ItemType Directory -Force -Path (Split-Path $QtConfig) | Out-Null
}
$lines = Set-IniKeys $lines 'System' ([ordered]@{
    'plugin_loader\default'       = 'false'
    'plugin_loader'               = 'true'
    'allow_plugin_loader\default' = 'true'
    'allow_plugin_loader'         = 'true'
})
[System.IO.File]::WriteAllText($QtConfig, (($lines -join "`r`n") + "`r`n"), $Utf8)
Write-Host "Enabled the 3GX plugin loader in $QtConfig"

Write-Host ''
Write-Host 'Done! Start Pokemon Alpha Sapphire (v1.4) in Azahar. In game, press SELECT for the oras-mmo menu.'
Write-Host 'If friends do not show up, check Emulation > Configure > System > "Enable 3GX plugin loader".'
