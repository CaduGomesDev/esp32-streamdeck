<#
  Converte a logo de um app (a mesma que o Windows mostra na notificacao) numa
  mascara de transparencia 32x32 pra tela de notificacoes da placa. Usado por
  notify-bridge.ps1 e bridge.ps1 (dot-source); quem inclui precisa ter a funcao
  Wait-WinRT definida.
#>
# Icone do app (a mesma logo que o Windows mostra na notificacao) reduzido pra
# uma mascara de transparencia 32x32 (1 byte por pixel) em base64 - o formato
# que a placa espera no campo "icon" (ICON_SIZE em src/Notification.cpp); ela
# pinta a mascara de branco. Guarda em cache por app pra nao refazer a cada
# notificacao. Retorna '' se nao achar.
Add-Type -AssemblyName System.Drawing
[Windows.Storage.Streams.RandomAccessStreamReference,Windows.Storage.Streams,ContentType=WindowsRuntime] | Out-Null
[Windows.Storage.Streams.IRandomAccessStreamWithContentType,Windows.Storage.Streams,ContentType=WindowsRuntime] | Out-Null
$IconSize = 32
$iconCache = @{}

# Icone do atalho do Menu Iniciar com o mesmo nome do app (apps classicos, que
# o WinRT nao entrega a logo). Retorna um Bitmap ou $null.
function Get-ShortcutIcon([string]$name) {
    $shell = New-Object -ComObject WScript.Shell
    $dirs = @("$env:ProgramData\Microsoft\Windows\Start Menu\Programs", "$env:APPDATA\Microsoft\Windows\Start Menu\Programs")
    $all = Get-ChildItem $dirs -Filter '*.lnk' -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.BaseName -like "*$name*" -and $_.BaseName -notmatch 'desinstal|uninstall' } |
        Sort-Object { if ($_.BaseName -eq $name) { 0 } else { 1 } }
    foreach ($lnk in $all) {
        $sc = $shell.CreateShortcut($lnk.FullName)
        $file = $sc.TargetPath
        $iconPath = ($sc.IconLocation -split ',')[0]
        if ($iconPath -and (Test-Path $iconPath)) { $file = $iconPath }
        if ($file -and (Test-Path $file)) {
            $icon = [System.Drawing.Icon]::ExtractAssociatedIcon($file)
            if ($icon) { return $icon.ToBitmap() }
        }
    }
    return $null
}
function Get-AppIcon($appInfo) {
    $name = $appInfo.DisplayInfo.DisplayName
    if ($iconCache.ContainsKey($name)) { return $iconCache[$name] }
    $b64 = ''
    try {
        $stream = $null
        $src = $null
        try {
            $ref = $appInfo.DisplayInfo.GetLogo((New-Object Windows.Foundation.Size 64, 64))
            $ras = Wait-WinRT ($ref.OpenReadAsync()) ([Windows.Storage.Streams.IRandomAccessStreamWithContentType])
            $stream = [System.IO.WindowsRuntimeStreamExtensions]::AsStreamForRead([Windows.Storage.Streams.IInputStream]$ras)
            $src = New-Object System.Drawing.Bitmap $stream
        } catch {
            $src = $null # apps classicos (ex.: Lightshot) nao entregam o stream: tenta o icone do atalho
        }
        if (-not $src) { $src = Get-ShortcutIcon $name }
        if (-not $src) { throw 'logo nao encontrada' }
        # Recorta pro quadrado que envolve a parte visivel (as logos do Windows
        # vem com muita margem transparente) e reduz pra IconSize x IconSize.
        $minX = $src.Width; $minY = $src.Height; $maxX = -1; $maxY = -1
        for ($y = 0; $y -lt $src.Height; $y++) {
            for ($x = 0; $x -lt $src.Width; $x++) {
                if ($src.GetPixel($x, $y).A -lt 32) { continue }
                if ($x -lt $minX) { $minX = $x }; if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }; if ($y -gt $maxY) { $maxY = $y }
            }
        }
        if ($maxX -lt 0) { $minX = 0; $minY = 0; $maxX = $src.Width - 1; $maxY = $src.Height - 1 }
        $side = [Math]::Max($maxX - $minX + 1, $maxY - $minY + 1)
        $cx = ($minX + $maxX + 1) / 2.0; $cy = ($minY + $maxY + 1) / 2.0
        $dst = New-Object System.Drawing.Bitmap $IconSize, $IconSize, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($dst)
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $srcRect = New-Object System.Drawing.RectangleF ($cx - $side / 2.0), ($cy - $side / 2.0), $side, $side
        $dstRect = New-Object System.Drawing.RectangleF 1, 1, ($IconSize - 2), ($IconSize - 2)
        $g.DrawImage($src, $dstRect, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)
        $g.Dispose()

        # Vira uma mascara de transparencia (a placa pinta de branco). Logo com
        # transparencia de verdade: a mascara e o proprio alfa. Logo que e um
        # quadrado chapado (icone com fundo colorido, ex.: WhatsApp): a mascara
        # e o quanto cada pixel difere da cor de fundo (a mais frequente), pra
        # sobrar so o desenho em branco e nao um quadrado branco.
        $n = $IconSize * $IconSize
        $px = New-Object 'System.Drawing.Color[]' $n
        $opaque = 0
        for ($i = 0; $i -lt $n; $i++) {
            $c = $dst.GetPixel($i % $IconSize, [int][Math]::Floor($i / $IconSize))
            $px[$i] = $c
            if ($c.A -ge 200) { $opaque++ }
        }
        # Fundo = a cor do meio das 4 bordas, se pelo menos 3 batem entre si
        # (quadrado/disco chapado); senao (sino preenchido, glifo solto...) a
        # mascara certa e o proprio alfa.
        $bgCol = $null
        if ($opaque -gt 0.5 * $n) {
            $edge = @(@(16, 2), @(16, 29), @(2, 16), @(29, 16)) | ForEach-Object { $px[$_[1] * $IconSize + $_[0]] } | Where-Object { $_.A -ge 200 }
            if (@($edge).Count -ge 3) {
                $avgR = ($edge | Measure-Object R -Average).Average
                $avgG = ($edge | Measure-Object G -Average).Average
                $avgB = ($edge | Measure-Object B -Average).Average
                $close = @($edge | Where-Object { [Math]::Max([Math]::Abs($_.R - $avgR), [Math]::Max([Math]::Abs($_.G - $avgG), [Math]::Abs($_.B - $avgB))) -le 60 }).Count
                if ($close -ge 3) { $bgCol = [System.Drawing.Color]::FromArgb([int]$avgR, [int]$avgG, [int]$avgB) }
            }
        }
        $bytes = New-Object byte[] $n
        $sum = 0
        for ($i = 0; $i -lt $n; $i++) {
            $c = $px[$i]
            $m = $c.A
            if ($bgCol) {
                $d = [Math]::Max([Math]::Abs($c.R - $bgCol.R), [Math]::Max([Math]::Abs($c.G - $bgCol.G), [Math]::Abs($c.B - $bgCol.B)))
                $m = [int][Math]::Min(255, [Math]::Max(0, ($d - 40) * 3) * $c.A / 255)
            }
            $bytes[$i] = [byte]$m
            $sum += $m
        }
        if ($sum -lt 0.03 * 255 * $n) { for ($i = 0; $i -lt $n; $i++) { $bytes[$i] = $px[$i].A } } # nada sobrou: usa so o alfa
        $b64 = [Convert]::ToBase64String($bytes)
        $dst.Dispose(); $src.Dispose(); if ($stream) { $stream.Dispose() }
    } catch {
        Write-Host "notify-bridge: sem icone pra '$name' ($($_.Exception.Message))"
    }
    $iconCache[$name] = $b64
    return $b64
}

