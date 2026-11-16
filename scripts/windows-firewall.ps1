#Requires -RunAsAdministrator
# Optional, explicit firewall setup for the portable Windows Wi-Fi mirror.
param([string]$ExePath = (Join-Path $PSScriptRoot 'porthole.exe'))
$ErrorActionPreference = 'Stop'
$program = (Resolve-Path -LiteralPath $ExePath).Path
$rules = @(
    @{ Name = 'Porthole-Mirror-TCP'; Protocol = 'TCP'; LocalPort = @('7000','7001','7100'); Program = $program },
    @{ Name = 'Porthole-Mirror-UDP'; Protocol = 'UDP'; LocalPort = @('6000','6001','7011'); Program = $program },
    @{ Name = 'Porthole-Mirror-mDNS'; Protocol = 'UDP'; LocalPort = '5353';
       Program = (Join-Path $env:SystemRoot 'System32\svchost.exe'); Service = 'Dnscache' }
)
foreach ($rule in $rules) {
    Get-NetFirewallRule -Name $rule.Name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    New-NetFirewallRule @rule -DisplayName $rule.Name -Direction Inbound -Action Allow `
        -Profile Private -RemoteAddress LocalSubnet | Out-Null
}
Write-Host 'Porthole Wi-Fi mirror is allowed from the local subnet on Private networks.'
