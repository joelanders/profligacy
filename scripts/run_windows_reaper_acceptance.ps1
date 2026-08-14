param(
    [Parameter(Mandatory = $true)][string]$Run,
    [Parameter(Mandatory = $true)][string]$RomRoot,
    [Parameter(Mandatory = $true)][string]$NvramRoot,
    [string]$ReaperRoot = 'C:\profligacy-tools\reaper-7.78-portable'
)

# SSH-safe REAPER acceptance: scan/instantiate, MIDI project save, 1x render,
# state restore, and a second instance. It does not claim a visual editor test.
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class ReaperPrompt {
  public delegate bool EnumProc(IntPtr hwnd, IntPtr lp);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lp);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr hwnd, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr hwnd);
  [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
  public static void DismissNoAudioDevice(uint targetPid) {
    EnumWindows((top,u) => { uint owner; GetWindowThreadProcessId(top,out owner); if(owner!=targetPid)return true;
      bool prompt=false; IntPtr no=IntPtr.Zero;
      EnumChildWindows(top,(child,v)=>{var text=new StringBuilder(1024);GetWindowText(child,text,text.Capacity);var cls=new StringBuilder(128);GetClassName(child,cls,cls.Capacity);if(text.ToString().Contains("not yet selected an audio device"))prompt=true;if(cls.ToString()=="Button"&&GetDlgCtrlID(child)==7)no=child;return true;},IntPtr.Zero);
      if(prompt&&no!=IntPtr.Zero)SendMessage(no,0x00F5,IntPtr.Zero,IntPtr.Zero);return true;
    },IntPtr.Zero);
  }
}
'@
Add-Type @'
using System;
using System.IO;
using System.Text;
public sealed class ProfligacyWavMetrics { public int Channels, Bits, Rate; public long Frames, Nonzero; public double Duration, Peak, Rms; }
public static class ProfligacyWavCheck {
  public static ProfligacyWavMetrics Analyze(string path) {
    using(var s=File.OpenRead(path)) using(var r=new BinaryReader(s)) {
      if(Encoding.ASCII.GetString(r.ReadBytes(4))!="RIFF")throw new Exception("not RIFF");r.ReadUInt32();if(Encoding.ASCII.GetString(r.ReadBytes(4))!="WAVE")throw new Exception("not WAVE");
      ushort format=0,channels=0,bits=0;uint rate=0,dataBytes=0;long dataAt=-1;
      while(s.Position+8<=s.Length){string id=Encoding.ASCII.GetString(r.ReadBytes(4));uint size=r.ReadUInt32();long next=s.Position+size+(size&1);if(id=="fmt "){format=r.ReadUInt16();channels=r.ReadUInt16();rate=r.ReadUInt32();r.ReadUInt32();r.ReadUInt16();bits=r.ReadUInt16();}else if(id=="data"){dataAt=s.Position;dataBytes=size;break;}s.Position=next;}
      if(dataAt<0||channels==0||rate==0||format!=1||(bits!=16&&bits!=24&&bits!=32))throw new Exception("unsupported WAV");
      s.Position=dataAt;int width=bits/8;long samples=dataBytes/width,nonzero=0;double scale=Math.Pow(2,bits-1),peak=0,sum=0;
      for(long i=0;i<samples;++i){int value;if(width==2)value=r.ReadInt16();else if(width==3){int a=r.ReadByte(),b=r.ReadByte(),c=r.ReadByte();value=a|(b<<8)|(c<<16);if((value&0x800000)!=0)value|=unchecked((int)0xff000000);}else value=r.ReadInt32();if(value!=0)++nonzero;double n=value/scale;peak=Math.Max(peak,Math.Abs(n));sum+=n*n;}
      return new ProfligacyWavMetrics{Channels=channels,Bits=bits,Rate=(int)rate,Frames=samples/channels,Nonzero=nonzero,Duration=(double)samples/channels/rate,Peak=peak,Rms=samples==0?0:Math.Sqrt(sum/samples)};
    }
  }
}
'@

$exe = Join-Path $ReaperRoot 'reaper.exe'
$ini = Join-Path $ReaperRoot 'REAPER.ini'
$out = Join-Path $Run 'reaper'
$receipts = Join-Path $Run 'receipts'
$project = Join-Path $out 'profligacy_smoke.rpp'
$wav = Join-Path $out 'reaper_render.wav'
$stagedVst3 = Join-Path $Run 'artifacts\Profligacy.vst3'
$vst3Binary = Join-Path $stagedVst3 'Contents\x86_64-win\Profligacy.vst3'
New-Item -ItemType Directory -Path $out, $receipts, $NvramRoot -Force | Out-Null
@"
[REAPER]
wnd_state=0
renderclosewhendone=4
vstpath=$(Split-Path -Parent $stagedVst3)
vstpath64=$(Split-Path -Parent $stagedVst3)
verchk=0
"@ | Set-Content -Encoding ASCII -LiteralPath $ini
$env:PROFLIGACY_REAPER_OUTDIR = $out
$env:PROPHECY_ROMPATH = $RomRoot
$env:PROPHECY_NVRAM = $NvramRoot
$env:PROPHECY_DSP_ENGINE = $null
$env:KPROP_DSP_PERFRAME = $null

function Invoke-ReaperScript([string[]]$Arguments, [string]$Report, [int]$Timeout = 90) {
    Remove-Item -Force -ErrorAction SilentlyContinue $Report
    $process = Start-Process -FilePath $exe -ArgumentList $Arguments -PassThru
    $deadline = [DateTime]::UtcNow.AddSeconds($Timeout)
    while ((-not (Test-Path -LiteralPath $Report)) -and [DateTime]::UtcNow -lt $deadline) {
        [ReaperPrompt]::DismissNoAudioDevice([uint32]$process.Id)
        Start-Sleep -Milliseconds 250
    }
    $ok = Test-Path -LiteralPath $Report
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    if (-not $ok) { throw "REAPER script produced no report: $Report" }
    $body = Get-Content -Raw -LiteralPath $Report
    if ($body -notmatch '(?m)^status=PASS$') { throw "REAPER stage failed:`n$body" }
    return $body
}

$stage1Report = Join-Path $out 'stage1_report.txt'
$stage1 = Invoke-ReaperScript -Arguments @('-newinst','-nosplash','-new',(Join-Path $PSScriptRoot 'reaper_windows_stage1.lua')) -Report $stage1Report
$projectText = Get-Content -Raw -LiteralPath $project
if ($projectText -match '(?m)^\s*RENDER_1X\s+.*$') {
    $projectText = $projectText -replace '(?m)^\s*RENDER_1X\s+.*$', '  RENDER_1X 1'
} else {
    $projectText = $projectText -replace '(?m)^(\s*RENDER_FMT.*)$', "$1`r`n  RENDER_1X 1"
}
Set-Content -Encoding ASCII -LiteralPath $project -Value $projectText
$renderOut = Join-Path $out 'offline_render.stdout.log'
$renderErr = Join-Path $out 'offline_render.stderr.log'
Remove-Item -Force -ErrorAction SilentlyContinue $wav, $renderOut, $renderErr
$render = Start-Process -FilePath $exe -ArgumentList @('-newinst','-nosplash','-renderproject',$project) `
    -PassThru -RedirectStandardOutput $renderOut -RedirectStandardError $renderErr
$deadline = [DateTime]::UtcNow.AddSeconds(180)
while ((-not $render.HasExited) -and [DateTime]::UtcNow -lt $deadline) {
    [ReaperPrompt]::DismissNoAudioDevice([uint32]$render.Id)
    Start-Sleep -Milliseconds 250
}
if (-not $render.HasExited) { Stop-Process -Id $render.Id -Force; throw 'REAPER render timed out' }
if (-not (Test-Path -LiteralPath $wav) -or -not (Select-String -Quiet -SimpleMatch 'Average speed:' -LiteralPath $renderOut)) { throw 'REAPER render incomplete' }
$metrics = [ProfligacyWavCheck]::Analyze($wav)
if ($metrics.Duration -lt 23.9 -or $metrics.Nonzero -le 1000 -or $metrics.Peak -le 0.000001 -or $metrics.Rms -le 0.00000001) { throw 'REAPER render is silent or incomplete' }
$stage2Report = Join-Path $out 'stage2_report.txt'
$stage2 = Invoke-ReaperScript -Arguments @('-newinst','-nosplash',$project,(Join-Path $PSScriptRoot 'reaper_windows_stage2.lua')) -Report $stage2Report

$receipt = [ordered]@{
    schema = 'profligacy-windows-reaper-acceptance-v1'
    created_utc = [DateTime]::UtcNow.ToString('o')
    reaper_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $exe).Hash
    artifact_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $vst3Binary).Hash
    stage1 = ($stage1.Trim() -split "`n")
    stage2 = ($stage2.Trim() -split "`n")
    render = [ordered]@{ channels=$metrics.Channels; bits=$metrics.Bits; sample_rate=$metrics.Rate; frames=$metrics.Frames; duration=$metrics.Duration; nonzero_samples=$metrics.Nonzero; peak=$metrics.Peak; rms=$metrics.Rms; sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $wav).Hash }
    success = $true
}
$receipt | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 (Join-Path $receipts 'reaper-acceptance.json')
$receipt | ConvertTo-Json -Depth 8
