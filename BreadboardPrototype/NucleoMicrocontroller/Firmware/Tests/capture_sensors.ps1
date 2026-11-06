param(
    [string]$Port = 'COM11',
    [ValidateRange(1, 3600)][int]$Seconds = 30,
    [string]$OutputPath,
    [switch]$Quiet,
    [ValidateSet('ALL', 'IMU', 'FLOW', 'TOF')][string]$Sensor = 'ALL',
    [ValidateRange(1, 20)][int]$DisplayHz = 1,
    [switch]$Compact
)
# The established capture command remains a compatible entry point.
& (Join-Path $PSScriptRoot 'capture_imu.ps1') @PSBoundParameters
