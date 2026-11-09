<#
  Ponte entre as notificacoes do Windows e a tela de notificacao do Stream
  Deck. Fica rodando no PC com o Stream Deck conectado por USB: a cada
  IntervalSec segundos confere as notificacoes atuais do Windows (toasts) e,
  pra cada uma nova, manda {"cmd":"notify","app":...,"title":...,"body":...}
  pela serial pra placa mostrar em tela cheia por alguns segundos.

  IMPORTANTE - permissao: na primeira vez que rodar, o Windows mostra um
  pedido de permissao ("Deixar o PowerShell acessar suas notificacoes?").
  Precisa aceitar, senao a leitura fica sempre vazia. Da pra conferir/revogar
  depois em Configuracoes > Sistema > Notificacoes > Acesso a notificacoes.

  IMPORTANTE - suporte: isto usa Windows.UI.Notifications.Management.
  UserNotificationListener num processo comum (nao empacotado). A Microsoft
  so documenta essa API pra apps empacotados (MSIX); em leitura por polling
  (GetNotificationsAsync) ela tambem funciona fora disso no Windows 11, mas
  isso nao e garantido pela Microsoft e pode nao funcionar em toda maquina/
  versao do Windows. Se o RequestAccessAsync vier sempre "Denied" ou o script
  nunca pegar nenhuma notificacao mesmo com permissao concedida, o proximo
  passo seria empacotar isto como um "sparse package" com identidade — bem
  mais trabalho, avise se for preciso chegar nesse ponto.

  Uso:
    powershell -ExecutionPolicy Bypass -File tools\notify-bridge.ps1
    ... -Port COM3        # forca a porta (senao detecta pelo VID 303A)
    ... -IntervalSec 3    # intervalo do polling (padrao 2s)

  A porta serial so aceita um programa por vez: feche este script antes de
  usar o configurador (Web Serial) e abra de novo depois.
#>
param(
    [string]$Port = '',
    [int]$IntervalSec = 2
)

$ErrorActionPreference = 'Stop'

# Carrega os tipos WinRT usados, projetados pro .NET (truque padrao pra
# acessar WinRT direto do Windows PowerShell 5.1, sem compilar nada).
[Windows.UI.Notifications.Management.UserNotificationListener,Windows.UI.Notifications.Management,ContentType=WindowsRuntime] | Out-Null
[Windows.UI.Notifications.Management.UserNotificationListenerAccessStatus,Windows.UI.Notifications.Management,ContentType=WindowsRuntime] | Out-Null
[Windows.UI.Notifications.NotificationKinds,Windows.UI.Notifications,ContentType=WindowsRuntime] | Out-Null
[Windows.UI.Notifications.UserNotification,Windows.UI.Notifications,ContentType=WindowsRuntime] | Out-Null
[Windows.UI.Notifications.KnownNotificationBindings,Windows.UI.Notifications,ContentType=WindowsRuntime] | Out-Null

# Helper pra "esperar" um IAsyncOperation do WinRT (PowerShell nao tem await
# nativo) - pega o metodo generico AsTask() via reflexao e bloqueia nele.
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
})[0]
function Wait-WinRT($asyncOp, [Type]$resultType) {
    $asTask = $asTaskGeneric.MakeGenericMethod($resultType)
    $task = $asTask.Invoke($null, @($asyncOp))
    $task.Wait() | Out-Null
    $task.Result
}

# A fonte da placa so tem ASCII: tira acentos (Notificacao, nao Notificação).
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

function Clip([string]$s, [int]$max) {
    $s = (ConvertTo-Ascii $s) -replace '\s+', ' '
    $s = $s.Trim()
    if ($s.Length -gt $max) { return $s.Substring(0, $max) }
    return $s
}

function Find-DeckPort {
    foreach ($d in Get-CimInstance Win32_PnPEntity) {
        if ($d.PNPDeviceID -like 'USB\VID_303A*' -and $d.Name -match '\((COM\d+)\)') {
            return $Matches[1]
        }
    }
    return $null
}

. (Join-Path $PSScriptRoot 'notify-icon.ps1')

Write-Host "notify-bridge: pedindo acesso as notificacoes do Windows..."
$listener = [Windows.UI.Notifications.Management.UserNotificationListener]::Current
$access = Wait-WinRT ($listener.RequestAccessAsync()) ([Windows.UI.Notifications.Management.UserNotificationListenerAccessStatus])
if ($access -ne [Windows.UI.Notifications.Management.UserNotificationListenerAccessStatus]::Allowed) {
    Write-Host "notify-bridge: acesso NEGADO ($access)."
    Write-Host "Va em Configuracoes > Sistema > Notificacoes > Acesso a notificacoes e libere este processo, depois rode de novo."
    exit 1
}
Write-Host "notify-bridge: acesso liberado."

# ids (UserNotification.Id, uint32) das notificacoes ja mandadas pra placa,
# pra nao repetir a cada polling.
$seen = New-Object 'System.Collections.Generic.HashSet[uint32]'

function Get-NewNotifications {
    $kinds = [Windows.UI.Notifications.NotificationKinds]::Toast
    $list = Wait-WinRT ($listener.GetNotificationsAsync($kinds)) ([System.Collections.Generic.IReadOnlyList[Windows.UI.Notifications.UserNotification]])

    $fresh = New-Object System.Collections.ArrayList
    $currentIds = New-Object 'System.Collections.Generic.HashSet[uint32]'
    foreach ($n in $list) {
        [void]$currentIds.Add($n.Id)
        if ($seen.Contains($n.Id)) { continue }
        [void]$seen.Add($n.Id)

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
    # esquece ids que sumiram da lista atual (notificacao dispensada no PC),
    # senao o HashSet so cresce pro resto da execucao
    foreach ($id in @($seen | Where-Object { -not $currentIds.Contains($_) })) {
        [void]$seen.Remove($id)
    }
    return $fresh
}

Write-Host "notify-bridge: monitorando (a cada ${IntervalSec}s)"
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
        Write-Host "notify-bridge: conectado em $portName"

        while ($true) {
            foreach ($note in (Get-NewNotifications)) {
                $payload = @{ cmd = 'notify'; app = $note.app; title = $note.title; body = $note.body; icon = $note.icon } | ConvertTo-Json -Compress
                $sp.WriteLine($payload)
                Write-Host "notify-bridge: [$($note.app)] $($note.title)"
                try { $sp.ReadLine() | Out-Null } catch [TimeoutException] {}
            }
            Start-Sleep -Seconds $IntervalSec
        }
    } catch {
        Write-Host "notify-bridge: $($_.Exception.Message) - tentando de novo em 3s"
    } finally {
        if ($sp) { try { $sp.Close() } catch {} }
    }
    Start-Sleep -Seconds 3
}
