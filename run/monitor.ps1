# ===========================================================================
#  monitor.ps1 -- a window you leave open to watch the research run
#
#  Refreshes every few seconds. Says plainly whether the worker is alive,
#  what it is chewing on, and how much is left.
#
#  The part that matters is the STALE warning. A model thinking hard and a
#  model that has died look identical from outside, so the worker writes a
#  heartbeat after every state change and this compares it against how long
#  that state should reasonably take.
# ===========================================================================
$ErrorActionPreference = "SilentlyContinue"
$root     = Split-Path -Parent $MyInvocation.MyCommand.Path
$ai       = Join-Path $root "ai"
$queue    = Join-Path $ai "queue"
$done     = Join-Path $ai "done"
$findings = Join-Path $ai "findings"
$statusF  = Join-Path $ai "status.json"
$logs     = Join-Path $root "logs"

$host.UI.RawUI.WindowTitle = "BENCH ONE - research monitor"

function Bar([int]$d, [int]$q) {
    $t = $d + $q
    if ($t -le 0) { return "" }
    $w = 40
    $f = [int](($d / $t) * $w)
    return "[" + ("#" * $f) + ("." * ($w - $f)) + "] $d/$t"
}

while ($true) {
    Clear-Host
    Write-Host ""
    Write-Host "  BENCH ONE  --  research monitor" -ForegroundColor Cyan
    Write-Host "  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" -ForegroundColor DarkGray
    Write-Host ""

    # ---- Ollama ----------------------------------------------------------
    $models = $null
    try { $models = Invoke-RestMethod -Uri "http://localhost:11434/api/tags" -TimeoutSec 3 } catch {}
    if ($models) {
        Write-Host "  Ollama    " -NoNewline
        Write-Host "up" -ForegroundColor Green -NoNewline
        Write-Host ", $($models.models.Count) models"
    } else {
        Write-Host "  Ollama    " -NoNewline
        Write-Host "DOWN" -ForegroundColor Red -NoNewline
        Write-Host "  -- run D:\start\START.bat"
    }

    # ---- worker ----------------------------------------------------------
    $q = @(Get-ChildItem "$queue\*.task").Count
    $d = @(Get-ChildItem "$done\*.task").Count
    $st = $null
    if (Test-Path $statusF) { try { $st = Get-Content $statusF -Raw | ConvertFrom-Json } catch {} }

    if ($st) {
        $age = [int]((Get-Date).ToUniversalTime() - (Get-Date "1970-01-01").AddSeconds($st.at)).TotalSeconds
        $state = $st.state
        $colour = "Green"
        # A single task on a 32B model can legitimately take many minutes. Only shout
        # once it has been silent far longer than any task has ever taken.
        if ($state -eq "working" -and $age -gt 2400) { $colour = "Red"; $state = "STALE ($age s)" }
        elseif ($state -eq "no-ollama")              { $colour = "Red" }
        elseif ($state -eq "finished")               { $colour = "Yellow" }
        Write-Host "  Worker    " -NoNewline
        Write-Host $state -ForegroundColor $colour -NoNewline
        Write-Host "   last beat $($st.at_human)"
        if ($st.task)  { Write-Host "            task:  $($st.task)" }
        if ($st.model) { Write-Host "            model: $($st.model)" }
        if ($st.secs)  { Write-Host "            last task took $($st.secs)s" }
    } else {
        Write-Host "  Worker    " -NoNewline
        Write-Host "not started" -ForegroundColor DarkGray -NoNewline
        Write-Host "  -- run ai-research.bat"
    }

    Write-Host ""
    Write-Host "  progress  $(Bar $d $q)"
    if ($q -eq 0 -and $d -gt 0) {
        Write-Host "            QUEUE IS DRY -- add .task files to run\ai\queue" -ForegroundColor Yellow
    }
    Write-Host ""

    # ---- what has come out ----------------------------------------------
    $f = @(Get-ChildItem "$findings\*.md" | Sort-Object LastWriteTime -Descending)
    Write-Host "  findings  $($f.Count)"
    foreach ($x in $f | Select-Object -First 6) {
        $mins = [int]((Get-Date) - $x.LastWriteTime).TotalMinutes
        Write-Host ("            {0,-42} {1,4} min ago" -f $x.BaseName, $mins) -ForegroundColor DarkGray
    }

    # ---- hardware test logs ---------------------------------------------
    $l = @(Get-ChildItem "$logs\*.txt" | Sort-Object LastWriteTime -Descending)
    if ($l.Count -gt 0) {
        Write-Host ""
        Write-Host "  test logs $($l.Count)"
        foreach ($x in $l | Select-Object -First 3) {
            $mins = [int]((Get-Date) - $x.LastWriteTime).TotalMinutes
            Write-Host ("            {0,-42} {1,4} min ago" -f $x.Name, $mins) -ForegroundColor DarkGray
        }
    }

    Write-Host ""
    Write-Host "  nothing here is a result until it is verified" -ForegroundColor DarkGray
    Write-Host "  refreshing every 5s, Ctrl+C to close" -ForegroundColor DarkGray
    Start-Sleep -Seconds 5
}
