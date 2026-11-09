<#
  Envia a imagem de fundo da tela de uso do Claude pra placa.

  Recorta a imagem pra 480x320 (cover, centralizada), converte pra RGB565 e
  comprime em RLE (a flash da placa e pequena: o bitmap cru tem 300 KB, e uma
  imagem quase chapada fica em poucos KB). Aceita PNG/JPG/BMP/GIF e tambem
  AVIF/WebP, que o Windows nao le sozinho: nesses casos o Chrome ou o Edge
  (headless) fazem a conversao.

  Uso:
    powershell -ExecutionPolicy Bypass -File tools\send-usage-bg.ps1 -Image foto.avif
    ... -Port COM10     # forca a porta (senao detecta pelo VID 303A)
    ... -Grid           # em vez da tela de uso, vira o fundo da GRADE de botoes
                        # (bitmap cru de 300 KB, put_bg); -Remove apaga esse fundo
    ... -DryRun         # so converte e mostra o tamanho, sem abrir a serial
    ... -Remove         # apaga o fundo da placa (volta pro fundo liso)

  A porta serial so aceita um programa por vez: feche a ponte (bridge.ps1) e
  o configurador antes.
#>
param(
    [string]$Image = '',
    [string]$Port = '',
    [switch]$Grid,
    [switch]$DryRun,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$W = 480; $H = 320

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
    $sp.DtrEnable = $true; $sp.NewLine = "`n"; $sp.ReadTimeout = 5000; $sp.WriteTimeout = 5000
    $sp.Open(); $sp.DiscardInBuffer()
    Write-Host "conectado em $portName"
    return $sp
}

function Send-Command($sp, $obj) {
    $sp.WriteLine(($obj | ConvertTo-Json -Compress))
    return $sp.ReadLine() | ConvertFrom-Json
}

if ($Remove) {
    $sp = Open-Deck
    try { $r = Send-Command $sp @{ cmd = $(if ($Grid) { 'delete_bg' } else { 'delete_usage_bg' }) }; Write-Host "resposta: $($r | ConvertTo-Json -Compress)" } finally { $sp.Close() }
    return
}

if (-not $Image) { throw 'Informe -Image <arquivo> (ou -Remove).' }
$Image = (Resolve-Path $Image).Path

# --- 1. imagem -> Bitmap 480x320 (cover) -------------------------------
$tmpPng = $null
$src = $Image
if ($Image -match '\.(avif|webp)$') {
    $browser = @(
        "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
        "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
        "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
        "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $browser) { throw 'AVIF/WebP precisa do Chrome ou Edge instalado (ou converta pra PNG).' }
    $work = Join-Path $env:TEMP "usage-bg-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory $work | Out-Null
    $uri = ([System.Uri]$Image).AbsoluteUri
    Set-Content (Join-Path $work 'bg.html') "<!doctype html><body style='margin:0;overflow:hidden'><img src='$uri' style='width:${W}px;height:${H}px;object-fit:cover;display:block'></body>" -Encoding UTF8
    $tmpPng = Join-Path $work 'bg.png'
    # o navegador escreve status no stderr; no PS 5.1 isso viraria erro fatal
    $ErrorActionPreference = 'Continue'
    & $browser --headless=new --disable-gpu --hide-scrollbars --window-size="$W,$H" --force-device-scale-factor=1 --allow-file-access-from-files "--screenshot=$tmpPng" ("file:///" + ((Join-Path $work 'bg.html') -replace '\\', '/')) 2>&1 | Out-Null
    $ErrorActionPreference = 'Stop'
    for ($i = 0; $i -lt 20 -and -not (Test-Path $tmpPng); $i++) { Start-Sleep -Milliseconds 250 }
    if (-not (Test-Path $tmpPng)) { throw 'o navegador nao conseguiu converter a imagem' }
    $src = $tmpPng
}

$orig = [System.Drawing.Image]::FromFile($src)
$bmp = New-Object System.Drawing.Bitmap $W, $H, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.InterpolationMode = 'HighQualityBicubic'
$scale = [Math]::Max($W / $orig.Width, $H / $orig.Height)
$dw = $orig.Width * $scale; $dh = $orig.Height * $scale
$g.DrawImage($orig, [single](($W - $dw) / 2), [single](($H - $dh) / 2), [single]$dw, [single]$dh)
$g.Dispose(); $orig.Dispose()
if ($tmpPng) { Remove-Item (Split-Path $tmpPng) -Recurse -Force -ErrorAction SilentlyContinue }

# --- 2. RGB565 + RLE ---------------------------------------------------
$data = $bmp.LockBits((New-Object System.Drawing.Rectangle 0, 0, $W, $H), 'ReadOnly', [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$raw = New-Object byte[] ($W * $H * 4)
[System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $raw, 0, $raw.Length)
$bmp.UnlockBits($data); $bmp.Dispose()

$out = New-Object System.IO.MemoryStream
$rawOut = New-Object System.IO.MemoryStream
$runColor = -1; $runLen = 0
for ($i = 0; $i -lt $raw.Length; $i += 4) {
    $b = $raw[$i]; $gg = $raw[$i + 1]; $r = $raw[$i + 2]   # BGRA
    $c = (($r -band 0xF8) -shl 8) -bor (($gg -band 0xFC) -shl 3) -bor ($b -shr 3)
    $rawOut.WriteByte($c -band 0xFF); $rawOut.WriteByte($c -shr 8)
    if ($c -eq $runColor -and $runLen -lt 255) { $runLen++; continue }
    if ($runLen -gt 0) { $out.WriteByte($runLen); $out.WriteByte($runColor -band 0xFF); $out.WriteByte($runColor -shr 8) }
    $runColor = $c; $runLen = 1
}
$out.WriteByte($runLen); $out.WriteByte($runColor -band 0xFF); $out.WriteByte($runColor -shr 8)
$bytes = if ($Grid) { $rawOut.ToArray() } else { $out.ToArray() }
Write-Host ("RLE: {0:N0} bytes (cru: {1:N0})" -f $bytes.Length, ($W * $H * 2))
if ($DryRun) { return }

# --- 3. envia ----------------------------------------------------------
$sp = Open-Deck
try {
    $r = Send-Command $sp @{ cmd = $(if ($Grid) { 'put_bg' } else { 'put_usage_bg' }); size = $bytes.Length }
    if (-not $r.ok) { throw "placa recusou: $($r | ConvertTo-Json -Compress)" }
    $sent = 0
    while ($sent -lt $bytes.Length) {
        $n = [Math]::Min(512, $bytes.Length - $sent)
        $sp.Write($bytes, $sent, $n); $sent += $n
        do { $r = $sp.ReadLine() | ConvertFrom-Json } while ($null -ne $r.received -and $r.received -lt $sent)
        if ($r.ok -eq $false) { throw "envio falhou: $($r | ConvertTo-Json -Compress)" }
    }
    if ($null -eq $r.written) { $r = $sp.ReadLine() | ConvertFrom-Json }
    Write-Host "enviado: $($r | ConvertTo-Json -Compress)"
} finally { $sp.Close() }
