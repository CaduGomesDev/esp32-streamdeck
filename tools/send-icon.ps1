<#
  Envia o icone de um botao pra placa (sem passar pelo configurador).

  Sem -Image desenha o logo do Health Server Monitor (o mesmo de
  health-server/vps/dashboard/public/logo.svg: quadrado verde com pulso). Com
  -Image aceita qualquer PNG/JPG/BMP/GIF (SVG nao: o GDI+ nao le).
  Converte pra RGBA 64x64 (ICON_SIZE do firmware) e grava em /icon<Index>.rgba.

  Uso:
    powershell -ExecutionPolicy Bypass -File tools\send-icon.ps1 -Index 1
    ... -Index 1 -Image logo.png
    ... -Port COM10     # forca a porta (senao detecta pelo VID 303A)
    ... -Remove         # apaga o icone do botao

  -Index e a posicao do botao na grade, 0-7 (o 2o card e o 1).
  A porta serial so aceita um programa por vez: feche a ponte (bridge.ps1) e
  o configurador antes.
#>
param(
    [Parameter(Mandatory)][ValidateRange(0, 7)][int]$Index,
    [string]$Image = '',
    [string]$Port = '',
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$SIZE = 64 # deve bater com Icon::SIZE no firmware

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
    $sp.DtrEnable = $true; $sp.NewLine = "`n"; $sp.ReadTimeout = 10000; $sp.WriteTimeout = 5000
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
    try { $r = Send-Command $sp @{ cmd = 'delete_icon'; index = $Index }; Write-Host "resposta: $($r | ConvertTo-Json -Compress)" } finally { $sp.Close() }
    return
}

# --- 1. imagem -> Bitmap 64x64 ARGB -------------------------------------
function New-Canvas([int]$px) {
    $b = New-Object System.Drawing.Bitmap $px, $px, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    return $b, $g
}

if ($Image) {
    $orig = [System.Drawing.Image]::FromFile((Resolve-Path $Image).Path)
    $big, $g = New-Canvas 256
    $g.InterpolationMode = 'HighQualityBicubic'
    $scale = [Math]::Max(256 / $orig.Width, 256 / $orig.Height) # cover, como o configurador
    $dw = $orig.Width * $scale; $dh = $orig.Height * $scale
    $g.DrawImage($orig, [single]((256 - $dw) / 2), [single]((256 - $dh) / 2), [single]$dw, [single]$dh)
    $orig.Dispose()
} else {
    # logo.svg do painel (viewBox 100x100) desenhado a mao em 256x256
    $big, $g = New-Canvas 256
    $k = 2.56
    $rr = 20 * $k * 2
    $rect = New-Object System.Drawing.RectangleF (4 * $k), (4 * $k), (92 * $k), (92 * $k)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($rect.X, $rect.Y, $rr, $rr, 180, 90)
    $path.AddArc($rect.Right - $rr, $rect.Y, $rr, $rr, 270, 90)
    $path.AddArc($rect.Right - $rr, $rect.Bottom - $rr, $rr, $rr, 0, 90)
    $path.AddArc($rect.X, $rect.Bottom - $rr, $rr, $rr, 90, 90)
    $path.CloseFigure()
    $g.FillPath((New-Object System.Drawing.SolidBrush ([System.Drawing.ColorTranslator]::FromHtml('#0b1419'))), $path)
    $g.DrawPath((New-Object System.Drawing.Pen ([System.Drawing.ColorTranslator]::FromHtml('#34d399')), (4 * $k)), $path)

    $pts = @(@(22, 50), @(35, 50), @(42, 30), @(58, 70), @(65, 50), @(78, 50)) |
        ForEach-Object { New-Object System.Drawing.PointF ([single]($_[0] * $k)), ([single]($_[1] * $k)) }
    $green = [System.Drawing.ColorTranslator]::FromHtml('#6ee7b7')
    # brilho (filter glow do svg): tracos mais largos e translucidos por baixo
    foreach ($glow in @(@(14, 30), @(10, 50), @(6, 255))) {
        $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb($glow[1], $green)), ($glow[0] * $k)
        $pen.StartCap = 'Round'; $pen.EndCap = 'Round'; $pen.LineJoin = 'Round'
        $g.DrawLines($pen, [System.Drawing.PointF[]]$pts)
    }
}
$g.Dispose()

$bmp, $g2 = New-Canvas $SIZE
$g2.InterpolationMode = 'HighQualityBicubic'
$g2.PixelOffsetMode = 'HighQuality'
$g2.DrawImage($big, 0, 0, $SIZE, $SIZE)
$g2.Dispose(); $big.Dispose()

# --- 2. BGRA (GDI+) -> RGBA cru -----------------------------------------
$data = $bmp.LockBits((New-Object System.Drawing.Rectangle 0, 0, $SIZE, $SIZE), 'ReadOnly', [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$bytes = New-Object byte[] ($SIZE * $SIZE * 4)
[System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
$bmp.UnlockBits($data); $bmp.Dispose()
for ($i = 0; $i -lt $bytes.Length; $i += 4) { $b = $bytes[$i]; $bytes[$i] = $bytes[$i + 2]; $bytes[$i + 2] = $b }

# --- 3. envia (mesmo protocolo do configurador: put_icon + blocos de 512) -
$sp = Open-Deck
try {
    $r = Send-Command $sp @{ cmd = 'put_icon'; index = $Index; size = $bytes.Length }
    if (-not $r.ok) { throw "placa recusou: $($r | ConvertTo-Json -Compress)" }
    $sent = 0
    while ($sent -lt $bytes.Length) {
        $n = [Math]::Min(512, $bytes.Length - $sent)
        $sp.Write($bytes, $sent, $n); $sent += $n
        do { $r = $sp.ReadLine() | ConvertFrom-Json } while ($null -ne $r.received -and $r.received -lt $sent -and $null -eq $r.written)
        if ($r.ok -eq $false) { throw "envio falhou: $($r | ConvertTo-Json -Compress)" }
    }
    while ($null -eq $r.written) { $r = $sp.ReadLine() | ConvertFrom-Json; if ($r.ok -eq $false) { throw "envio falhou: $($r | ConvertTo-Json -Compress)" } }
    Write-Host "icone do botao $Index enviado: $($r | ConvertTo-Json -Compress)"
} finally { $sp.Close() }
