param(
    [string]$Port = 'COM11',
    [ValidateRange(1, 3600)][int]$Seconds = 30,
    [string]$OutputPath,
    [switch]$Quiet,
    [ValidateSet('ALL', 'IMU', 'FLOW', 'TOF')][string]$Sensor = 'ALL',
    [ValidateRange(1, 20)][int]$DisplayHz = 1,
    [switch]$Compact
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'FormatSensorReading.ps1')
if (-not $OutputPath) {
    $OutputPath = Join-Path (Split-Path -Parent $PSScriptRoot) ('build/hardware-tests/sensors-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.log')
}
$absolutePath = [System.IO.Path]::GetFullPath($OutputPath)
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $absolutePath) | Out-Null
$serial = [System.IO.Ports.SerialPort]::new($Port, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.ReadTimeout = 250
$serial.Handshake = [System.IO.Ports.Handshake]::None
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$writer = $null
$capturedCount = 0
$flowCount = 0
$unexpectedCount = 0
$nextNoDataNotice = 5.0
$tofCount = 0
$lastDisplayBucket = @{ IMU = -1L; FLOW = -1L; TOF = -1L }
$watch = [System.Diagnostics.Stopwatch]::new()
try {
    $serial.Open() # Close Serial Monitor first: only one reader can own a COM port.
    $writer = [System.IO.StreamWriter]::new($absolutePath, $false, [System.Text.UTF8Encoding]::new($false))
    $writer.AutoFlush = $true
    $serial.DiscardInBuffer()
    # Opening a streaming port can land halfway through a line. Synchronize
    # once so that a partial first line is not misreported as different firmware.
    try { $null = $serial.ReadLine() } catch [System.TimeoutException] { }
    $watch.Start()
    Write-Host ("Streaming {0} at 115200 baud for {1} seconds. Log: {2}" -f $Port, $Seconds, $absolutePath)
    Write-Host ("Terminal display: up to {0} updates/second per sensor. All received lines are saved." -f $DisplayHz)
    while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
        try {
            $line = $serial.ReadLine().TrimEnd("`r")
            if ($line.Length -gt 0) { $writer.WriteLine($line) }
            $kind = $null
            if ($line.StartsWith('IMU ')) {
                ++$capturedCount
                $kind = 'IMU'
            } elseif ($line.StartsWith('FLOW ')) {
                ++$flowCount
                $kind = 'FLOW'
            } elseif ($line.StartsWith('TOF ')) {
                ++$tofCount
                $kind = 'TOF'
            } elseif ($line.Length -gt 0) {
                if ($unexpectedCount -eq 0) {
                    Write-Warning 'Serial data is arriving, but it does not start with IMU, FLOW or TOF. The board may be running different firmware. Unexpected lines are shown as RAW and saved to the log.'
                }
                ++$unexpectedCount
                if (-not $Quiet) { Write-Output ('RAW: ' + $line) }
            }
            if ($kind -and -not $Quiet -and ($Sensor -eq 'ALL' -or $Sensor -eq $kind)) {
                # Keep reading/logging at full speed; select one reading
                # per time bucket independently for each sensor.
                $bucket = [long][Math]::Floor($watch.Elapsed.TotalSeconds * $DisplayHz)
                if ($bucket -ne $lastDisplayBucket[$kind]) {
                    if ($Compact) { Write-Output $line }
                    else { Write-Output (Format-SensorReading -Line $line) }
                    $lastDisplayBucket[$kind] = $bucket
                }
            }
        } catch [System.TimeoutException] { }
        if ($capturedCount -eq 0 -and $flowCount -eq 0 -and $tofCount -eq 0 -and $unexpectedCount -eq 0 -and $watch.Elapsed.TotalSeconds -ge $nextNoDataNotice) {
            Write-Warning ("No complete serial lines received on {0} yet. Check that the MCU is running and the correct board/port is selected." -f $Port)
            $nextNoDataNotice += 5.0
        }
    }
} finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
    if ($null -ne $writer) { $writer.Dispose() }
    $watch.Stop()
}
Write-Output ("Captured {0} IMU lines, {1} FLOW lines, {5} TOF lines and {2} other serial lines in {3:N2} seconds: {4}" -f $capturedCount, $flowCount, $unexpectedCount, $watch.Elapsed.TotalSeconds, $absolutePath, $tofCount)
if ($Sensor -eq 'TOF' -and $tofCount -eq 0) { throw 'No TOF lines arrived. Flash the three-sensor firmware to enable time-of-flight output.' }
if ($Sensor -eq 'FLOW' -and $flowCount -eq 0) { throw 'No FLOW lines arrived. Flash the combined sensor firmware to enable optical-flow output.' }
if ($Sensor -eq 'IMU' -and $capturedCount -eq 0) { throw 'No IMU lines arrived. Check that the combined or MPU6050 firmware is running.' }
if ($capturedCount -eq 0 -and $flowCount -eq 0 -and $tofCount -eq 0) {
    if ($unexpectedCount -gt 0) { throw 'Serial data was received, but no recognized sensor lines. Flash the combined sensor firmware to the selected board.' }
    throw 'No serial lines received. Check the selected port, board power, and that firmware is running.'
}
