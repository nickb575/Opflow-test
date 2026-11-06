function Format-SensorReading {
    param([Parameter(Mandatory = $true)][string]$Line)

    $parts = $Line -split '\s+'
    $sensorName = switch ($parts[0]) {
        'IMU' { 'IMU' }
        'FLOW' { 'OPTICAL FLOW' }
        'TOF' { 'TIME OF FLIGHT' }
        default { return $Line }
    }
    $labels = @{
        t_ms = 'MCU time (ms)'
        sample_ms = 'Sample time (ms)'
        seq = 'Sample sequence'
        ax_mg = 'Acceleration X (mg)'
        ay_mg = 'Acceleration Y (mg)'
        az_mg = 'Acceleration Z (mg)'
        gx_mdps = 'Gyroscope X (mdeg/s)'
        gy_mdps = 'Gyroscope Y (mdeg/s)'
        gz_mdps = 'Gyroscope Z (mdeg/s)'
        dx = 'X motion (counts)'
        dy = 'Y motion (counts)'
        mot = 'Motion detected (0/1)'
        squal = 'Tracking quality (SQUAL)'
        shutter = 'Shutter (raw)'
        rawsum = 'Image sum (raw)'
        rawmin = 'Image minimum (raw)'
        rawmax = 'Image maximum (raw)'
        obs = 'Observation register'
        status = 'Status'
        id = 'Device ID'
        rev = 'Revision ID'
        inv = 'Inverse device ID'
        config = 'Configuration'
        errors = 'Sensor errors'
        attempts = 'Initialization attempts'
        i2c = 'I2C error bits'
        spi = 'SPI error bits'
        range_mm = 'Range (mm)'
        amp = 'Signal amplitude (LSB)'
        quality = 'Signal quality (%)'
        chip = 'Chip ID'
        argus = 'Argus status code'
        hw = 'Peripheral init error bits'
    }
    $formatted = [System.Collections.Generic.List[string]]::new()
    $formatted.Add(('--- {0} ---' -f $sensorName))
    foreach ($field in $parts | Select-Object -Skip 1) {
        $separator = $field.IndexOf('=')
        if ($separator -lt 1) { continue }
        $key = $field.Substring(0, $separator)
        $value = $field.Substring($separator + 1)
        $label = if ($labels.ContainsKey($key)) { $labels[$key] } else { $key }
        $formatted.Add(('{0,-25} : {1}' -f $label, $value))
    }
    return ([Environment]::NewLine + ($formatted -join [Environment]::NewLine) + [Environment]::NewLine)
}
