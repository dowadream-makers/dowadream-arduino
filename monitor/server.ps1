# 도와드림 레이더 모니터 - COM 포트 -> 웹 중계 서버 (설치 필요 없음)
# 사용: powershell -ExecutionPolicy Bypass -File server.ps1 [-Port COM8] [-Http 8765]
param([string]$Port = 'COM8', [int]$Http = 8765, [int]$Baud = 115200)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$utf8 = New-Object System.Text.UTF8Encoding($false)

$lines = New-Object System.Collections.Generic.List[object]
$script:seq = 0
$script:partial = ''
$script:sp = $null
$script:lastOpenTry = [DateTime]::MinValue
$script:status = 'closed'

function Add-Line([string]$l, [string]$kind = 'serial') {
  $script:seq++
  $lines.Add([pscustomobject]@{ seq = $script:seq; t = [DateTimeOffset]::Now.ToUnixTimeMilliseconds(); kind = $kind; line = $l })
  if ($lines.Count -gt 6000) { $lines.RemoveRange(0, 1000) }
}

function Open-Port {
  if ($script:sp -and $script:sp.IsOpen) { return }
  if (([DateTime]::Now - $script:lastOpenTry).TotalSeconds -lt 2) { return }
  $script:lastOpenTry = [DateTime]::Now
  try {
    $p = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
    $p.Encoding = $utf8
    $p.DtrEnable = $false; $p.RtsEnable = $false   # 열 때 보드가 리셋되지 않게
    $p.Open()
    $script:sp = $p; $script:status = 'open'
    Add-Line "$Port 연결됨 ($Baud bps)" 'sys'
    Write-Host "[$(Get-Date -f HH:mm:ss)] $Port 연결됨"
  } catch {
    if ($script:status -ne 'error') { Add-Line "$Port 열기 실패: $($_.Exception.Message)" 'sys' }
    $script:status = 'error'
    Write-Host "[$(Get-Date -f HH:mm:ss)] $Port 열기 실패: $($_.Exception.Message) (시리얼 모니터가 열려 있으면 닫아 주세요)"
  }
}

function Send-Bytes($ctx, [byte[]]$bytes, [string]$type) {
  $ctx.Response.ContentType = $type
  $ctx.Response.Headers.Add('Cache-Control', 'no-store')
  $ctx.Response.ContentLength64 = $bytes.Length
  $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
  $ctx.Response.Close()
}

function Handle($ctx) {
  $path = $ctx.Request.Url.AbsolutePath
  if ($path -eq '/' -or $path -eq '/index.html') {
    Send-Bytes $ctx ([IO.File]::ReadAllBytes((Join-Path $root 'index.html'))) 'text/html; charset=utf-8'; return
  }
  if ($path -eq '/data') {
    $since = 0; [void][int]::TryParse($ctx.Request.QueryString['since'], [ref]$since)
    $new = @()
    if ($lines.Count -gt 0) {
      $start = [Math]::Max(0, $since - $lines[0].seq + 1)
      if ($start -lt $lines.Count) { $new = $lines.GetRange($start, [Math]::Min(1500, $lines.Count - $start)).ToArray() }
    }
    $obj = [ordered]@{ port = $Port; status = $script:status; seq = $script:seq; lines = $new }
    $json = ConvertTo-Json -InputObject $obj -Depth 4 -Compress
    Send-Bytes $ctx ($utf8.GetBytes($json)) 'application/json; charset=utf-8'; return
  }
  if ($path -eq '/reset' -and $ctx.Request.HttpMethod -eq 'POST') {
    $ok = $false
    if ($script:sp -and $script:sp.IsOpen) {
      $script:sp.RtsEnable = $true; Start-Sleep -Milliseconds 200; $script:sp.RtsEnable = $false
      Add-Line '보드 재시작 요청 (센서에 AT+START 다시 전송됨)' 'sys'; $ok = $true
    }
    Send-Bytes $ctx ($utf8.GetBytes("{`"ok`":$($ok.ToString().ToLower())}")) 'application/json'; return
  }
  if ($path -eq '/send' -and $ctx.Request.HttpMethod -eq 'POST') {
    $ok = $false; $txt = $ctx.Request.QueryString['t']
    if ($txt -and $script:sp -and $script:sp.IsOpen) { $script:sp.Write($txt); Add-Line "PC -> 보드: $txt" 'sys'; $ok = $true }
    Send-Bytes $ctx ($utf8.GetBytes("{`"ok`":$($ok.ToString().ToLower())}")) 'application/json'; return
  }
  $ctx.Response.StatusCode = 404; $ctx.Response.Close()
}

$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://localhost:$Http/")
$listener.Start()
Write-Host "레이더 모니터: 크롬에서 http://localhost:$Http 를 여세요. 끝내려면 이 창에서 Ctrl+C"
Open-Port

$task = $listener.GetContextAsync()
try {
  while ($listener.IsListening) {
    Open-Port
    if ($script:sp -and $script:sp.IsOpen) {
      try {
        $chunk = $script:sp.ReadExisting()
        if ($chunk) {
          $script:partial += $chunk
          while (($i = $script:partial.IndexOf("`n")) -ge 0) {
            $l = $script:partial.Substring(0, $i).TrimEnd("`r")
            $script:partial = $script:partial.Substring($i + 1)
            if ($l.Length) { Add-Line $l }
          }
        }
      } catch {
        Add-Line "$Port 연결 끊김: $($_.Exception.Message)" 'sys'
        try { $script:sp.Close() } catch {}
        $script:sp = $null; $script:status = 'error'
      }
    }
    if ($task.Wait(40)) {
      $ctx = $task.Result
      $task = $listener.GetContextAsync()
      try { Handle $ctx } catch { try { $ctx.Response.StatusCode = 500; $ctx.Response.Close() } catch {} }
    }
  }
} finally {
  if ($script:sp) { $script:sp.Close() }
  $listener.Stop()
}
