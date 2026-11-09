# Janelinha do uso do Claude (5h / 7 dias). Fica sempre por cima, sem fechar sozinha.
# Nao toca no token: quem consulta a API e o claude_usage.py, que grava um JSON
# so com porcentagens e textos; esta janela apenas le esse JSON.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing

$mutex = New-Object System.Threading.Mutex($false, 'Local\ClaudeUsageWindow')
if (-not $mutex.WaitOne(0)) { exit }   # ja aberta: nao dobra as consultas (o endpoint limita a taxa)

. (Join-Path $PSScriptRoot 'claude-usage-daemon.ps1')

# $proc so existe se esta janela iniciou o monitor; se a ponte do Stream Deck
# ja o tinha iniciado, ele continua rodando quando a janela fechar.
try { $proc = Start-ClaudeUsageDaemon } catch {
    [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Claude uso') | Out-Null
    exit 1
}
$snapFile = $script:ClaudeUsageSnapshot

$bg = [System.Drawing.Color]::FromArgb(24, 24, 27)
$fg = [System.Drawing.Color]::FromArgb(236, 236, 240)
$dim = [System.Drawing.Color]::FromArgb(150, 150, 160)
$track = [System.Drawing.Color]::FromArgb(52, 52, 58)
$accent = [System.Drawing.Color]::FromArgb(217, 119, 87)
$warn = [System.Drawing.Color]::FromArgb(240, 180, 60)

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Claude - uso'
$form.TopMost = $true
$form.FormBorderStyle = 'FixedSingle'
$form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.ClientSize = New-Object System.Drawing.Size(340, 190)
$form.BackColor = $bg
$form.ForeColor = $fg
$form.Font = New-Object System.Drawing.Font('Segoe UI', 10)

function New-Label($x, $y, $w, $h, $color) {
    $l = New-Object System.Windows.Forms.Label
    $l.Location = New-Object System.Drawing.Point($x, $y)
    $l.Size = New-Object System.Drawing.Size($w, $h)
    $l.ForeColor = $color
    $form.Controls.Add($l)
    return $l
}

$rows = @()
foreach ($i in 0..1) {
    $y = 16 + $i * 62
    $title = New-Label 16 $y 150 20 $fg
    $pct = New-Label 200 $y 124 20 $fg
    $pct.TextAlign = 'TopRight'
    $trackPanel = New-Object System.Windows.Forms.Panel
    $trackPanel.Location = New-Object System.Drawing.Point(16, ($y + 24))
    $trackPanel.Size = New-Object System.Drawing.Size(308, 10)
    $trackPanel.BackColor = $track
    $fill = New-Object System.Windows.Forms.Panel
    $fill.Size = New-Object System.Drawing.Size(0, 10)
    $fill.BackColor = $accent
    $trackPanel.Controls.Add($fill)
    $form.Controls.Add($trackPanel)
    $reset = New-Label 16 ($y + 36) 308 18 $dim
    $reset.Font = New-Object System.Drawing.Font('Segoe UI', 8.5)
    $rows += [pscustomobject]@{ Title = $title; Pct = $pct; Fill = $fill; Reset = $reset }
}
$age = New-Label 16 142 308 18 $dim
$age.Font = New-Object System.Drawing.Font('Segoe UI', 8.5)
$warning = New-Label 16 160 308 28 $warn
$warning.Font = New-Object System.Drawing.Font('Segoe UI', 8.5)

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 2000
$timer.Add_Tick({
    if (-not (Test-Path $snapFile)) { $age.Text = 'consultando...'; return }
    try { $snap = Get-Content $snapFile -Raw -Encoding UTF8 | ConvertFrom-Json } catch { return }
    for ($i = 0; $i -lt 2; $i++) {
        $w = $snap.windows[$i]; $r = $rows[$i]
        $r.Title.Text = $w.label
        $r.Reset.Text = $w.reset
        if ($null -eq $w.percent) {
            $r.Pct.Text = '--'; $r.Fill.Width = 0
        } else {
            $r.Pct.Text = ('{0:N1}%' -f [double]$w.percent)
            $r.Fill.Width = [int](308 * [math]::Min(100, [math]::Max(0, [double]$w.percent)) / 100)
        }
    }
    $age.Text = $snap.age
    $warning.Text = if ($snap.warning) { $snap.warning } else { '' }
})
$timer.Start()

$form.Add_FormClosed({
    $timer.Stop()
    if ($proc -and -not $proc.HasExited) { $proc.Kill() }
    $mutex.ReleaseMutex()
})

[System.Windows.Forms.Application]::Run($form)
