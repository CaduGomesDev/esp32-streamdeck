# Helpers (dot-source) para manter UM unico claude_usage.py rodando em segundo
# plano, gravando o snapshot (so porcentagens e textos, sem token) que a janela
# do PC e a ponte do Stream Deck leem. Um so processo consulta a API: o
# endpoint limita a taxa, entao dois monitores dobrariam as consultas.

$script:ClaudeUsageSnapshot = Join-Path $env:LOCALAPPDATA 'claude-usage\snapshot.json'

function Find-ClaudePython {
    $real = Get-Command pythonw.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.Source -notlike '*\WindowsApps\*' } | Select-Object -First 1
    if ($real) { return $real.Source }
    $pio = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pythonw.exe'
    if (Test-Path $pio) { return $pio }
    throw 'Python nao encontrado (nem o do PlatformIO).'
}

function Test-ClaudeUsageDaemon {
    $found = Get-CimInstance Win32_Process -Filter "Name LIKE 'python%'" |
        Where-Object { $_.CommandLine -like '*claude_usage.py*--snapshot-file*' }
    return [bool]$found
}

# Inicia o monitor se ainda nao houver um. Devolve o Process quando foi este
# chamador que iniciou (quem inicia encerra), ou $null se ja existia um.
function Start-ClaudeUsageDaemon {
    if (Test-ClaudeUsageDaemon) { return $null }
    $python = Find-ClaudePython
    New-Item -ItemType Directory -Force (Split-Path $script:ClaudeUsageSnapshot) | Out-Null
    Remove-Item $script:ClaudeUsageSnapshot -ErrorAction SilentlyContinue
    $script = Join-Path $PSScriptRoot 'claude_usage.py'
    return Start-Process $python -ArgumentList @("`"$script`"", '--snapshot-file', "`"$script:ClaudeUsageSnapshot`"") `
        -WindowStyle Hidden -PassThru
}
