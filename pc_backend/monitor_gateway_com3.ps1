$ErrorActionPreference = 'Stop'
$portName = 'COM3'
$baudRate = 115200

$port = [System.IO.Ports.SerialPort]::new($portName, $baudRate, 'None', 8, 'One')
$port.NewLine = "`n"
$port.ReadTimeout = 250
$port.Open()
Write-Host "Monitoring $portName at $baudRate 8N1. Press Ctrl+C to stop." -ForegroundColor Cyan
try {
    while ($port.IsOpen) {
        try {
            $line = $port.ReadLine()
            if ($line.Length -gt 0) {
                Write-Host $line
            }
        } catch [System.TimeoutException] {
        }
    }
} finally {
    if ($port.IsOpen) { $port.Close() }
    $port.Dispose()
}
