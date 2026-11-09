<#
  Prepara um GIF/video pra tela de descanso da placa: 480x320 e dentro do limite
  de tamanho (MAX_GIF_SIZE em src/Screensaver.h, ~1000 KB).

  A placa NAO redimensiona o GIF: desenha no tamanho original, cortando o que
  passar de 480x320. Este script usa o ffmpeg pra:
    - escalar/recortar com "cover" (Lanczos), igual ao configurador;
    - gerar uma paleta otima pro conteudo todo (stats_mode=diff);
    - gravar so a area que mudou entre quadros (diff_mode=rectangle), que e o que
      mais reduz o arquivo;
    - testar da melhor qualidade pra pior (mais quadros e cores primeiro) e ficar
      com a primeira combinacao que cabe.

  Uso:
    powershell -ExecutionPolicy Bypass -File tools\prepare-gif.ps1 -In original.gif
    ... -Out tela.gif       # padrao: <nome>-deck.gif ao lado do original
    ... -MaxKB 900          # limite (padrao 950, a placa aceita ate 1000)
    ... -Dither none        # none = bordas limpas/menor; bayer = degrade mais suave
    ... -Seconds 4          # corta o loop em N segundos

  Depois envie o arquivo gerado pelo configurador (secao GIF > Enviar).
#>
param(
    [Parameter(Mandatory)][string]$In,
    [string]$Out = '',
    [int]$MaxKB = 950,
    [ValidateSet('bayer', 'none')][string]$Dither = 'bayer',
    [double]$Seconds = 0
)

$ErrorActionPreference = 'Stop'
$W = 480; $H = 320

if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) { throw 'ffmpeg nao encontrado no PATH' }
$In = (Resolve-Path $In).Path
if (-not $Out) { $Out = Join-Path (Split-Path $In) ([IO.Path]::GetFileNameWithoutExtension($In) + '-deck.gif') }

$rate = (& ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate -of default=nw=1:nk=1 $In | Select-Object -First 1)
$n, $d = $rate -split '/'
$srcFps = if ($d -and [double]$d -gt 0) { [double]$n / [double]$d } else { 10 }
Write-Host ("origem: {0} ({1:N2} fps) | alvo: {2}x{3}, ate {4} KB" -f (Split-Path $In -Leaf), $srcFps, $W, $H, $MaxKB)

$dz = if ($Dither -eq 'bayer') { 'dither=bayer:bayer_scale=5' } else { 'dither=none' }
$trim = if ($Seconds -gt 0) { @('-t', "$Seconds") } else { @() }
$limit = $MaxKB * 1024
$tmp = Join-Path $env:TEMP ("prep-gif-" + [guid]::NewGuid().ToString('N') + '.gif')
$found = $false
try {
    # "step" = usa 1 a cada N quadros (mantem o tempo total, so reduz a taxa)
    foreach ($step in 1, 2, 3, 4, 6) {
        foreach ($colors in 256, 192, 128, 96, 64) {
            $fps = $srcFps / $step
            $vf = "select='not(mod(n\,$step))',setpts=N/($fps*TB)," +
                  "scale=${W}:${H}:force_original_aspect_ratio=increase:flags=lanczos,crop=${W}:${H},setsar=1," +
                  "split[a][b];[a]palettegen=max_colors=${colors}:stats_mode=diff[p];" +
                  "[b][p]paletteuse=${dz}:diff_mode=rectangle"
            & ffmpeg -v error -y -i $In @trim -vf $vf -loop 0 $tmp
            if ($LASTEXITCODE -ne 0) { throw 'ffmpeg falhou' }
            $size = (Get-Item $tmp).Length
            Write-Host ("  {0:N1} fps, {1,3} cores: {2:N0} KB" -f $fps, $colors, ($size / 1024))
            if ($size -le $limit) { $found = $true; break }
        }
        if ($found) { break }
    }
    if (-not $found) { throw "nada coube em $MaxKB KB - use -Seconds (loop mais curto)" }
    Move-Item $tmp $Out -Force
} finally { if (Test-Path $tmp) { Remove-Item $tmp -Force } }

$info = & ffprobe -v error -select_streams v:0 -show_entries stream=width,height,nb_frames -of csv=p=0 $Out
Write-Host ("pronto: {0} ({1}; {2:N0} KB)" -f $Out, $info, ((Get-Item $Out).Length / 1024))
