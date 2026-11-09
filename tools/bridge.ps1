<#
  Ponte unica entre o PC e o Stream Deck: status dos servidores + notificacoes
  do Windows, na MESMA conexao serial (a porta so aceita um programa por vez,
  entao rodar status-bridge.ps1 e notify-bridge.ps1 ao mesmo tempo nao
  funciona — este script faz as duas coisas junto).

  Fica rodando no PC com o Stream Deck conectado por USB:
  - Quando voce toca no botao "Status dos servidores", a placa manda
    {"event":"status_request"} pela serial; este script consulta a API do
    painel (GET /api/pcs) e responde com {"cmd":"status",...}.
  - A cada NotifyIntervalSec segundos, confere as notificacoes atuais do
    Windows e manda as novas com {"cmd":"notify",...} pra placa mostrar.

  IMPORTANTE - permissao de notificacoes: na primeira vez que rodar, o
  Windows pede permissao ("Deixar o PowerShell acessar suas notificacoes?").
  Precisa aceitar, senao a leitura fica sempre vazia. Da pra conferir/revogar
  depois em Configuracoes > Sistema > Notificacoes > Acesso a notificacoes.

  IMPORTANTE - suporte: a leitura de notificacoes usa Windows.UI.Notifications.
  Management.UserNotificationListener num processo comum (nao empacotado).
  A Microsoft so documenta essa API pra apps empacotados (MSIX); em leitura
  por polling (GetNotificationsAsync) ela tambem funciona fora disso no
  Windows 11, mas isso nao e garantido pela Microsoft nem testado em toda
  versao do Windows.

  Uso:
    powershell -ExecutionPolicy Bypass -File tools\bridge.ps1
    ... -Port COM3                 # forca a porta (senao detecta pelo VID 303A)
    ... -Url https://host/api/pcs  # API de status (ou env HSM_URL); sem isso a tela de status mostra erro
    ... -User fulano -Password xxx # Basic Auth do painel (ou env HSM_USER / HSM_PASSWORD)
    ... -NotifyIntervalSec 3       # intervalo do polling de notificacoes (padrao 2s)
    ... -NoNotify                  # so status, sem ler notificacoes do Windows

  A porta serial so aceita um programa por vez: feche este script antes de
  usar o configurador (Web Serial) e abra de novo depois.
#>
param(
    [string]$Url = $env:HSM_URL,
    [string]$User = $env:HSM_USER,
    [string]$Password = $env:HSM_PASSWORD,
    [string]$Port = '',
    [int]$NotifyIntervalSec = 2,
    [switch]$NoNotify,
    [switch]$NoUsage
)

$ErrorActionPreference = 'Stop'

# O painel pede HTTP Basic Auth. Usuario/senha vem de -User/-Password ou das
# variaveis de ambiente HSM_USER/HSM_PASSWORD (assim a senha nao fica no repo).
$AuthHeaders = @{}
if ($User) {
    $AuthHeaders['Authorization'] = 'Basic ' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes("${User}:${Password}"))
}

. (Join-Path $PSScriptRoot 'claude-usage-daemon.ps1')
. (Join-Path $PSScriptRoot 'notify-icon.ps1')

# ---------------------------------------------------------------------
# comum
# ---------------------------------------------------------------------

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

function Find-DeckPort {
    foreach ($d in Get-CimInstance Win32_PnPEntity) {
        if ($d.PNPDeviceID -like 'USB\VID_303A*' -and $d.Name -match '\((COM\d+)\)') {
            return $Matches[1]
        }
    }
    return $null
}

# ---------------------------------------------------------------------
# status dos servidores
# ---------------------------------------------------------------------

# Invoke-RestMethod -TimeoutSec so limita a conexao: se o servidor para de
# mandar a resposta no meio (Wi-Fi/VPN caiu), o PS 5.1 espera o ReadWriteTimeout
# padrao de 5 MINUTOS — e como a ponte e de uma thread so, status, uso do Claude
# e notificacoes congelam juntos. HttpClient.Timeout cobre a requisicao inteira.
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

# ---------------------------------------------------------------------
# uso do Claude (desativado com -NoUsage)
# Quem consulta a API (e le o token) e o claude_usage.py; esta ponte so le o
# snapshot que ele grava (porcentagens e textos) e repassa pra placa.
# ---------------------------------------------------------------------

function Get-UsagePayload {
    $f = Get-Item $script:ClaudeUsageSnapshot -ErrorAction SilentlyContinue
    # O monitor regrava o snapshot a cada ~30s; arquivo parado = monitor morto.
    if (-not $f -or ((Get-Date) - $f.LastWriteTime).TotalSeconds -gt 120) {
        try { $script:usageProc = Start-ClaudeUsageDaemon } catch { }
        return (@{ cmd = 'usage'; error = 'monitor do Claude parado' } | ConvertTo-Json -Compress)
    }
    try {
        $snap = Get-Content $f.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        $w = @($snap.windows | ForEach-Object {
            @{ l = ConvertTo-Ascii $_.label; p = $_.percent; r = $_.reset_sec }
        })
        $payload = @{ cmd = 'usage'; w = $w; a = $snap.age_sec }
        if ($snap.warning) { $payload.warn = (ConvertTo-Ascii $snap.warning) }
        return ($payload | ConvertTo-Json -Compress -Depth 4)
    } catch {
        return (@{ cmd = 'usage'; error = 'snapshot ilegivel' } | ConvertTo-Json -Compress)
    }
}

# ---------------------------------------------------------------------
# notificacoes do Windows (desativado com -NoNotify)
# ---------------------------------------------------------------------

$script:listener = $null
$script:seenNotifyIds = $null

function Initialize-NotifyListener {
    [Windows.UI.Notifications.Management.UserNotificationListener,Windows.UI.Notifications.Management,ContentType=WindowsRuntime] | Out-Null
    [Windows.UI.Notifications.Management.UserNotificationListenerAccessStatus,Windows.UI.Notifications.Management,ContentType=WindowsRuntime] | Out-Null
    [Windows.UI.Notifications.NotificationKinds,Windows.UI.Notifications,ContentType=WindowsRuntime] | Out-Null
    [Windows.UI.Notifications.UserNotification,Windows.UI.Notifications,ContentType=WindowsRuntime] | Out-Null
    [Windows.UI.Notifications.KnownNotificationBindings,Windows.UI.Notifications,ContentType=WindowsRuntime] | Out-Null

    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    $script:asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    })[0]

    Write-Host "bridge: pedindo acesso as notificacoes do Windows..."
    $script:listener = [Windows.UI.Notifications.Management.UserNotificationListener]::Current
    $access = Wait-WinRT ($script:listener.RequestAccessAsync()) ([Windows.UI.Notifications.Management.UserNotificationListenerAccessStatus])
    if ($access -ne [Windows.UI.Notifications.Management.UserNotificationListenerAccessStatus]::Allowed) {
        Write-Host "bridge: acesso as notificacoes NEGADO ($access) - seguindo so com status."
        Write-Host "Va em Configuracoes > Sistema > Notificacoes > Acesso a notificacoes pra liberar."
        $script:listener = $null
        return
    }
    Write-Host "bridge: acesso as notificacoes liberado."
    $script:seenNotifyIds = New-Object 'System.Collections.Generic.HashSet[uint32]'
}

# timeoutMs -1 = sem limite (o pedido de permissao espera o usuario responder)
function Wait-WinRT($asyncOp, [Type]$resultType, [int]$timeoutMs = -1) {
    $asTask = $script:asTaskGeneric.MakeGenericMethod($resultType)
    $task = $asTask.Invoke($null, @($asyncOp))
    if (-not $task.Wait($timeoutMs)) { throw 'o Windows nao respondeu a leitura de notificacoes' }
    $task.Result
}

function Clip([string]$s, [int]$max) {
    $s = (ConvertTo-Ascii $s) -replace '\s+', ' '
    $s = $s.Trim()
    if ($s.Length -gt $max) { return $s.Substring(0, $max) }
    return $s
}

function Get-NewNotifications {
    if (-not $script:listener) { return @() }
    $kinds = [Windows.UI.Notifications.NotificationKinds]::Toast
    $list = Wait-WinRT ($script:listener.GetNotificationsAsync($kinds)) ([System.Collections.Generic.IReadOnlyList[Windows.UI.Notifications.UserNotification]]) 10000

    $fresh = New-Object System.Collections.ArrayList
    $currentIds = New-Object 'System.Collections.Generic.HashSet[uint32]'
    foreach ($n in $list) {
        [void]$currentIds.Add($n.Id)
        if ($script:seenNotifyIds.Contains($n.Id)) { continue }
        [void]$script:seenNotifyIds.Add($n.Id)

        try {
            $binding = $n.Notification.Visual.GetBinding([Windows.UI.Notifications.KnownNotificationBindings]::ToastGeneric)
            if (-not $binding) { continue }
            $texts = @($binding.GetTextElements() | ForEach-Object { $_.Text })
            if ($texts.Count -eq 0) { continue }
            $title = $texts[0]
            $body = ($texts | Select-Object -Skip 1) -join ' - '
            $app = $n.AppInfo.DisplayInfo.DisplayName
            [void]$fresh.Add(@{ app = (Clip $app 30); title = (Clip $title 90); body = (Clip $body 500); icon = (Get-AppIcon $n.AppInfo) })
        } catch {
            continue
        }
    }
    foreach ($id in @($script:seenNotifyIds | Where-Object { -not $currentIds.Contains($_) })) {
        [void]$script:seenNotifyIds.Remove($id)
    }
    return $fresh
}

# ---------------------------------------------------------------------
# loop principal
# ---------------------------------------------------------------------

if (-not $NoNotify) { Initialize-NotifyListener }

$script:usageProc = $null
if (-not $NoUsage) {
    try { $script:usageProc = Start-ClaudeUsageDaemon } catch { Write-Host "bridge: uso do Claude desativado - $($_.Exception.Message)" }
}

if ($Url) { Write-Host "bridge: servidor $Url" } else { Write-Host 'bridge: sem API de status (defina HSM_URL ou use -Url); a tela de status vai mostrar erro' }
try {
while ($true) {
    $sp = $null
    try {
        $portName = if ($Port) { $Port } else { Find-DeckPort }
        if (-not $portName) { throw 'Stream Deck nao encontrado na USB' }

        $sp = New-Object System.IO.Ports.SerialPort $portName, 115200, 'None', 8, 'One'
        $sp.DtrEnable = $true
        $sp.NewLine = "`n"
        $sp.ReadTimeout = 500
        $sp.WriteTimeout = 3000
        $sp.Open()
        Write-Host "bridge: conectado em $portName"

        $nextNotifyCheck = Get-Date
        while ($true) {
            try {
                $line = $sp.ReadLine()
                if ($line -match 'status_request') {
                    $sp.WriteLine((Get-StatusPayload))
                } elseif ($line -match 'usage_request' -and -not $NoUsage) {
                    $sp.WriteLine((Get-UsagePayload))
                }
            } catch [TimeoutException] { }

            if ($script:listener -and (Get-Date) -ge $nextNotifyCheck) {
                foreach ($note in (Get-NewNotifications)) {
                    $payload = @{ cmd = 'notify'; app = $note.app; title = $note.title; body = $note.body; icon = $note.icon } | ConvertTo-Json -Compress
                    $sp.WriteLine($payload)
                    Write-Host "bridge: notificacao [$($note.app)] $($note.title)"
                    try { $sp.ReadLine() | Out-Null } catch [TimeoutException] {}
                }
                $nextNotifyCheck = (Get-Date).AddSeconds($NotifyIntervalSec)
            }
        }
    } catch {
        Write-Host "bridge: $($_.Exception.Message) - tentando de novo em 3s"
    } finally {
        if ($sp) { try { $sp.Close() } catch {} }
    }
    Start-Sleep -Seconds 3
}
} finally {
    # encerra o monitor do Claude so se foi esta ponte que o iniciou
    if ($script:usageProc -and -not $script:usageProc.HasExited) { $script:usageProc.Kill() }
}
