<#
  Ponte entre o servidor do esp32-pc-monitor e a tela de status do Stream Deck.

  Fica rodando no PC com o Stream Deck conectado por USB. Quando voce toca no
  botao "Status dos servidores", a placa manda {"event":"status_request"} pela
  serial; este script consulta a API do painel (GET /api/pcs) e responde com
  {"cmd":"status",...} para a placa desenhar os cards.

  Nao precisa instalar nada (usa System.IO.Ports do Windows PowerShell 5.1) e
  nao mexe no servidor: so le a API publica do painel.

  Uso:
    powershell -ExecutionPolicy Bypass -File tools\status-bridge.ps1
    ... -Port COM3                 # forca a porta (senao detecta pelo VID 303A)
    ... -Url https://host/api/pcs  # API de status (ou env HSM_URL); obrigatorio
    ... -User fulano -Password xxx # Basic Auth do painel (ou env HSM_USER / HSM_PASSWORD)
    ... -DryRun                    # so mostra o que enviaria, sem abrir a serial

  A porta serial so aceita um programa por vez: feche este script antes de
  usar o configurador (Web Serial) e abra de novo depois.
#>
param(
    [string]$Url = $env:HSM_URL,
    [string]$User = $env:HSM_USER,
    [string]$Password = $env:HSM_PASSWORD,
    [string]$Port = '',
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

# O painel pede HTTP Basic Auth. Usuario/senha vem de -User/-Password ou das
# variaveis de ambiente HSM_USER/HSM_PASSWORD (assim a senha nao fica no repo).
$AuthHeaders = @{}
if ($User) {
    $AuthHeaders['Authorization'] = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("${User}:${Password}"))
}

# A fonte da placa so tem ASCII: tira acentos (Servico, nao Serviço).
function ConvertTo-Ascii([string]$s) {
    if (-not $s) { return '' }
    $sb = New-Object System.Text.StringBuilder
    foreach ($ch in $s.Normalize([Text.NormalizationForm]::FormD).ToCharArray()) {
        $cat = [Globalization.CharUnicodeInfo]::GetUnicodeCategory($ch)
        if ($cat -ne [Globalization.UnicodeCategory]::NonSpacingMark -and [int]$ch -lt 128) {
            [void]$sb.Append($ch)
        }
    }
    $sb.ToString()
}

# Invoke-RestMethod -TimeoutSec nao cobre a leitura da resposta (o PS 5.1 espera
# 5 minutos se o servidor parar no meio); HttpClient.Timeout cobre tudo.
Add-Type -AssemblyName System.Net.Http
$script:http = New-Object System.Net.Http.HttpClient
$script:http.Timeout = [TimeSpan]::FromSeconds(8)
foreach ($k in $AuthHeaders.Keys) { [void]$script:http.DefaultRequestHeaders.TryAddWithoutValidation($k, $AuthHeaders[$k]) }

function Get-StatusPayload {
    try {
        $bytes = $script:http.GetByteArrayAsync($Url).GetAwaiter().GetResult()
        $resp = [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
        $pcs = @($resp.pcs | ForEach-Object {
            @{
                n  = ConvertTo-Ascii $_.nome
                h  = ConvertTo-Ascii $_.host
                ip = [string]$_.ip
                on = [int][bool]$_.online
                c  = [int]$_.cpu
                r  = [int]$_.ram
                d  = [int]$_.disco
                u  = [int64]$_.uptime
                s  = [int]$_.semSinalSeg
                # campos que o painel novo mostra (SO, eventos 24h, streak, historico, online desde)
                so  = (ConvertTo-Ascii $_.so)
                e   = [int]$_.eventos24h
                st  = [int]$_.streak
                stt = [int]$_.streakTotal
                hi  = (@($_.historico | ForEach-Object { if ($_) { '1' } else { '0' } }) -join '')
                sn  = $(if ($_.desde) { [DateTimeOffset]::FromUnixTimeMilliseconds([int64]$_.desde).ToLocalTime().ToString('dd/MM HH:mm') } else { '' })
            }
        })
        @{ cmd = 'status'; broker = $(if ($null -ne $resp.servidor) { [bool]$resp.servidor } else { [bool]$resp.mqttOnline }); pcs = $pcs } | ConvertTo-Json -Compress -Depth 4
    } catch {
        @{ cmd = 'status'; error = 'servidor inacessivel' } | ConvertTo-Json -Compress
    }
}

function Find-DeckPort {
    foreach ($d in Get-CimInstance Win32_PnPEntity) {
        if ($d.PNPDeviceID -like 'USB\VID_303A*' -and $d.Name -match '\((COM\d+)\)') {
            return $Matches[1]
        }
    }
    return $null
}

if ($DryRun) {
    Get-StatusPayload
    return
}

if ($Url) { Write-Host "status-bridge: servidor $Url" } else { Write-Host 'status-bridge: sem API de status (defina HSM_URL ou use -Url); a tela de status vai mostrar erro' }
while ($true) {
    $sp = $null
    try {
        $portName = if ($Port) { $Port } else { Find-DeckPort }
        if (-not $portName) { throw 'Stream Deck nao encontrado na USB' }

        $sp = New-Object System.IO.Ports.SerialPort $portName, 115200, 'None', 8, 'One'
        $sp.DtrEnable = $true
        $sp.NewLine = "`n"
        $sp.ReadTimeout = 1000
        $sp.WriteTimeout = 3000
        $sp.Open()
        Write-Host "status-bridge: conectado em $portName"

        while ($true) {
            try { $line = $sp.ReadLine() } catch [TimeoutException] { continue }
            if ($line -match 'status_request') {
                $sp.WriteLine((Get-StatusPayload))
            }
        }
    } catch {
        Write-Host "status-bridge: $($_.Exception.Message) - tentando de novo em 3s"
    } finally {
        if ($sp) { try { $sp.Close() } catch {} }
    }
    Start-Sleep -Seconds 3
}
