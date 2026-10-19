# SPDX-License-Identifier: GPL-3.0-or-later
#
# OBS AirPlay diagnostics for Windows: run while OBS is open if devices
# cannot see or connect to the receiver. Needs no administrator rights and
# changes nothing.
#
#   powershell -ExecutionPolicy Bypass -File diagnose.ps1
#
# The output names your network adapter, local IP addresses and the AirPlay
# devices on your network: read it before posting it publicly.

$ErrorActionPreference = 'SilentlyContinue'
$Issues = 'https://github.com/LZlamian/OBS-AirPlay-Plugin/issues'
$script:Errors = 0
$script:Warnings = 0

function Ok([string]$Text)   { Write-Host '  ok    ' -ForegroundColor Green -NoNewline; Write-Host $Text }
function Warn([string]$Text) { Write-Host '  warn  ' -ForegroundColor Yellow -NoNewline; Write-Host $Text; $script:Warnings++ }
function Fail([string]$Text) { Write-Host '  fail  ' -ForegroundColor Red -NoNewline; Write-Host $Text; $script:Errors++ }
function Info([string]$Text) { Write-Host "        $Text" }
function Section([string]$Text) { Write-Host ''; Write-Host $Text -ForegroundColor White }

Section 'Windows'
$os = Get-CimInstance Win32_OperatingSystem
$build = [int]$os.BuildNumber
Ok "$($os.Caption) (build $build)"
if ($build -ge 17763) {
    Ok 'Windows can advertise the receiver itself (DNS service discovery, Windows 10 1809 or later)'
} else {
    Warn 'this Windows cannot advertise AirPlay receivers itself: Bonjour is required'
}

Section 'OBS'
$obsFolder = (Get-ItemProperty 'HKLM:\SOFTWARE\OBS Studio').'(default)'
if (-not $obsFolder) { $obsFolder = Join-Path $env:ProgramFiles 'obs-studio' }
$obsExe = Join-Path $obsFolder 'bin\64bit\obs64.exe'
if (Test-Path $obsExe) {
    Ok "OBS $((Get-Item $obsExe).VersionInfo.ProductVersion) at $obsFolder"
} else {
    Fail "OBS not found at $obsFolder"
}
if (Get-Process obs64) { Ok 'OBS is running' } else { Warn 'OBS is not running: start it, then run this again' }

Section 'Plugin'
$plugin = Join-Path $env:ProgramData 'obs-studio\plugins\obs-airplay'
$dll = Join-Path $plugin 'bin\64bit\obs-airplay.dll'
if (Test-Path $dll) {
    $version = Get-Content (Join-Path $plugin 'VERSION.txt') -First 1
    Ok "installed at $plugin (version $version)"
} else {
    Fail "obs-airplay.dll not found in $plugin (install it with the installer or the zip)"
}
$legacy = Join-Path $obsFolder 'obs-plugins\64bit\obs-airplay.dll'
if (Test-Path $legacy) { Warn "a second copy is in OBS's own plugin folder: $legacy (delete one)" }

Section 'Discovery (Bonjour / mDNS)'
$bonjour = Get-Service 'Bonjour Service'
if ($bonjour) {
    if ($bonjour.Status -eq 'Running') { Ok 'Bonjour is installed and running (the plugin uses it)' }
    else { Warn "Bonjour is installed but $($bonjour.Status): start the Bonjour Service or uninstall Bonjour" }
} else {
    Info 'Bonjour is not installed: the plugin uses the Windows DNS service instead'
    $dnscache = Get-Service Dnscache
    if ($dnscache.Status -eq 'Running') { Ok 'the DNS Client service is running' }
    else { Fail "the DNS Client service is $($dnscache.Status)" }
    $mdns = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\Dnscache\Parameters').EnableMDNS
    if ($mdns -eq 0) { Fail 'mDNS is turned off (EnableMDNS = 0 in the DNS Client parameters)' }
}
$name = 'OBS AirPlay'
$settings = Join-Path $env:APPDATA 'obs-studio\plugin_config\obs-airplay\settings.json'
if (Test-Path $settings) {
    $saved = (Get-Content $settings -Raw | ConvertFrom-Json).server_name
    if ($saved) { $name = $saved }
}
$found = $false
$dnssd = Get-Command dns-sd.exe
if ($dnssd) {
    $scan = Join-Path $env:TEMP "obs-airplay-scan-$PID.txt"
    $process = Start-Process $dnssd.Source -ArgumentList '-B', '_airplay._tcp' -NoNewWindow -PassThru -RedirectStandardOutput $scan
    Start-Sleep -Seconds 5
    Stop-Process -Id $process.Id
    $found = Select-String -Path $scan -SimpleMatch $name -Quiet
    Remove-Item $scan
} else {
    $answers = Resolve-DnsName -Name '_airplay._tcp.local' -Type PTR -QuickTimeout
    $found = [bool]($answers | Where-Object { $_.NameHost -like "$name.*" })
}
if ($found) { Ok "`"$name`" is advertised" }
else { Warn "`"$name`" was not seen on the network (is OBS running with the plugin loaded?)" }

Section 'Network'
foreach ($connection in Get-NetConnectionProfile) {
    $text = "$($connection.InterfaceAlias): $($connection.NetworkCategory) network"
    if ($connection.NetworkCategory -eq 'Public') {
        Warn "$text (firewall rules for private networks do not apply: set it to Private)"
    } else {
        Ok $text
    }
}
Get-NetIPAddress -AddressFamily IPv4 -PrefixOrigin Dhcp, Manual |
    Where-Object { $_.IPAddress -notlike '169.254.*' } |
    ForEach-Object { Info "$($_.InterfaceAlias): $($_.IPAddress)" }

Section 'Firewall'
$rule = Get-NetFirewallRule -DisplayName 'OBS AirPlay'
if ($rule -and $rule.Enabled -eq 'True') {
    Ok "rule `"OBS AirPlay`" allows OBS on $($rule.Profile) networks"
} else {
    Warn 'no "OBS AirPlay" rule: allow OBS through Windows Firewall (the installer adds one)'
}
$blocks = Get-NetFirewallApplicationFilter | Where-Object { $_.Program -like '*obs64.exe' } |
    Get-NetFirewallRule | Where-Object { $_.Enabled -eq 'True' -and $_.Action -eq 'Block' -and $_.Direction -eq 'Inbound' }
foreach ($block in $blocks) { Fail "an inbound rule blocks OBS: `"$($block.DisplayName)`" ($($block.Profile))" }

Section 'Ports (the receiver uses TCP 7000, or 7001 if 7000 is taken)'
foreach ($port in 7000, 7001) {
    $listener = Get-NetTCPConnection -LocalPort $port -State Listen | Select-Object -First 1
    if ($listener) {
        $owner = (Get-Process -Id $listener.OwningProcess).ProcessName
        if ($owner -eq 'obs64') { Ok "TCP ${port}: OBS" } else { Warn "TCP $port is used by $owner" }
    } else {
        Info "TCP ${port}: free"
    }
}

Section 'OBS log'
$logs = Join-Path $env:APPDATA 'obs-studio\logs'
$latest = Get-ChildItem $logs -Filter *.txt | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($latest) {
    Ok "latest log: $($latest.Name)"
    if (Select-String -Path $latest.FullName -SimpleMatch 'OBS AirPlay Plugin loaded' -Quiet) {
        Ok 'the plugin loaded'
    } else {
        Fail 'the plugin did not load in that session'
    }
    Select-String -Path $latest.FullName -Pattern 'UxPlay integration (started|active)|Failed to start UxPlay|AirPlay discovery|\[LOAD\]' |
        Select-Object -Last 6 | ForEach-Object { Info $_.Line }
} else {
    Warn 'no OBS logs found'
}

Section 'Summary'
if ($script:Errors -eq 0 -and $script:Warnings -eq 0) { Ok 'nothing wrong found' }
else { Info "$($script:Errors) problem(s), $($script:Warnings) warning(s)" }
Info "Report problems at $Issues (read the output first: it names your network and devices)."
if ($script:Errors -gt 0) { exit 1 }
