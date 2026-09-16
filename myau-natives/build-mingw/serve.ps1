$root = Join-Path $PSScriptRoot "payload"
$port = 8765

$existing = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "[serve] port $port is already in use by pid $($existing.OwningProcess -join ',')"
    Write-Host "[serve] close the other server window (or stop that process) and start this script again."
    Read-Host "press enter to exit"
    exit 1
}

$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $port)
try {
    $listener.Start()
} catch {
    Write-Host "[serve] failed to listen on port ${port}: $_"
    Read-Host "press enter to exit"
    exit 1
}

Write-Host "serving $root"
Write-Host "http://127.0.0.1:$port/  (close this window to stop)"
try {
    while ($true) {
        $client = $null
        try {
            $client = $listener.AcceptTcpClient()
            $stream = $client.GetStream()
            $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::ASCII, $false, 8192, $true)
            $requestLine = $reader.ReadLine()
            while (-not $reader.EndOfStream -and $reader.ReadLine() -ne "") {}
            if (-not $requestLine) { throw "empty request" }
            $path = (($requestLine -split ' ')[1] -split '\?')[0].TrimStart('/')
            if ($path -eq "") { $path = "changelog.txt" }
            $file = [System.IO.Path]::GetFullPath((Join-Path $root ($path -replace '/', '\')))
            if (-not $file.StartsWith($root) -or -not (Test-Path $file -PathType Leaf)) {
                $body = [System.Text.Encoding]::ASCII.GetBytes("404 not found")
                $header = "HTTP/1.1 404 Not Found`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n"
            } else {
                $body = [System.IO.File]::ReadAllBytes($file)
                $header = "HTTP/1.1 200 OK`r`nContent-Length: $($body.Length)`r`nContent-Type: application/octet-stream`r`nConnection: close`r`n`r`n"
            }
            $bytes = [System.Text.Encoding]::ASCII.GetBytes($header)
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Write($body, 0, $body.Length)
            $stream.Close()
        } catch {
            Write-Host "req error: $_"
        } finally {
            if ($client -ne $null) {
                $client.Close()
            }
        }
    }
} finally {
    $listener.Stop()
}
