<#
  Envia o GIF da tela de descanso pra placa (sem passar pelo configurador).

  Uso:
    powershell -ExecutionPolicy Bypass -File tools\send-gif.ps1 -Gif data\screensaver.gif
    ... -Port COM8      # forca a porta (senao detecta pelo VID 303A)
    ... -Remove         # apaga o GIF da placa

  Mesmo protocolo do configurador: put_gif + blocos de 512 bytes, esperando o
  ack cumulativo de cada bloco. A porta serial so aceita um programa por vez:
  feche a ponte (bridge.ps1) e o configurador antes. O GIF antigo e apagado
  antes do envio, entao uma falha no meio deixa a placa sem GIF.
#>
param(
    [string]$Gif = '',
    [string]$Port = '',
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'

function Find-DeckPort {
    foreach ($d in Get-CimInstance Win32_PnPEntity) {
        if ($d.PNPDeviceID -like 'USB\VID_303A*' -and $d.Name -match '\((COM\d+)\)') { return $Matches[1] }
    }
    return $null
}

function Open-Deck {
    $portName = if ($Port) { $Port } else { Find-DeckPort }
    if (-not $portName) { throw 'Stream Deck nao encontrado na USB' }
    $sp = New-Object System.IO.Ports.SerialPort $portName, 115200, 'None', 8, 'One'
    $sp.DtrEnable = $true; $sp.NewLine = "`n"; $sp.ReadTimeout = 15000; $sp.WriteTimeout = 5000
    $sp.Open(); $sp.DiscardInBuffer()
    Write-Host "conectado em $portName"
    return $sp
}

function Send-Command($sp, $obj) {
    $sp.WriteLine(($obj | ConvertTo-Json -Compress))
    return $sp.ReadLine() | ConvertFrom-Json
}

if (-not $Remove -and -not $Gif) { throw 'informe -Gif <arquivo> ou -Remove' }

$sp = Open-Deck
try {
    if ($Remove) {
        $r = Send-Command $sp @{ cmd = 'delete_gif' }
        Write-Host "resposta: $($r | ConvertTo-Json -Compress)"
        return
    }

    $bytes = [IO.File]::ReadAllBytes((Resolve-Path $Gif).Path)
    $info = Send-Command $sp @{ cmd = 'ping' }
    Write-Host "placa: $($info | ConvertTo-Json -Compress)"
    if ($info.max_gif_size -and $bytes.Length -gt $info.max_gif_size) {
        throw "GIF com $($bytes.Length) bytes passa do limite da placa ($($info.max_gif_size))"
    }

    $r = Send-Command $sp @{ cmd = 'put_gif'; size = $bytes.Length }
    if (-not $r.ok) { throw "placa recusou: $($r | ConvertTo-Json -Compress)" }
    $sent = 0; $lastPct = -1
    while ($sent -lt $bytes.Length) {
        $n = [Math]::Min(512, $bytes.Length - $sent)
        $sp.Write($bytes, $sent, $n); $sent += $n
        do { $r = $sp.ReadLine() | ConvertFrom-Json } while ($null -ne $r.received -and $r.received -lt $sent -and $null -eq $r.written)
        if ($r.ok -eq $false) { throw "envio falhou: $($r | ConvertTo-Json -Compress)" }
        $pct = [int](100 * $sent / $bytes.Length)
        if ($pct -ge $lastPct + 10) { Write-Host "  $pct%"; $lastPct = $pct }
    }
    while ($null -eq $r.written) { $r = $sp.ReadLine() | ConvertFrom-Json; if ($r.ok -eq $false) { throw "envio falhou: $($r | ConvertTo-Json -Compress)" } }
    Write-Host "gif enviado: $($r | ConvertTo-Json -Compress)"
} finally { $sp.Close() }
