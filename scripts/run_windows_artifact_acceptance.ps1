param(
    [Parameter(Mandatory = $true)][string]$RunId,
    [string]$Repo = (Split-Path -Parent $PSScriptRoot),
    [string]$Root = 'C:\profligacy-acceptance',
    [string]$SteinbergValidator = '',
    [string]$RomRoot = '',
    [string]$NvramRoot = ''
)

# Validate copied shipping bytes, not linked targets. Each RunId is immutable so
# a later rebuild cannot silently replace the artifact for which receipts exist.
$ErrorActionPreference = 'Stop'
$run = Join-Path $Root $RunId
if (Test-Path -LiteralPath $run) { throw "Refusing existing acceptance run: $run" }
$artifacts = Join-Path $run 'artifacts'
$tools = Join-Path $run 'tools'
$receipts = Join-Path $run 'receipts'
$logs = Join-Path $run 'logs'
New-Item -ItemType Directory -Path $artifacts, $tools, $receipts, $logs | Out-Null

$sourceStandalone = Join-Path $Repo 'build-windows\ProphecyPlugin_artefacts\Release\Standalone\Profligacy.exe'
$sourceVst3 = Join-Path $Repo 'build-windows\ProphecyPlugin_artefacts\Release\VST3\Profligacy.vst3'
$artifactHost = Join-Path $Repo 'build-windows\ProphecyArtifactHost_artefacts\Release\ProphecyArtifactHost.exe'
$stagedStandalone = Join-Path $artifacts 'Profligacy.exe'
$stagedVst3 = Join-Path $artifacts 'Profligacy.vst3'
Copy-Item -LiteralPath $sourceStandalone -Destination $stagedStandalone
Copy-Item -LiteralPath $sourceVst3 -Destination $stagedVst3 -Recurse
$vst3Binary = Join-Path $stagedVst3 'Contents\x86_64-win\Profligacy.vst3'

$pluginvalUrl = 'https://github.com/Tracktion/pluginval/releases/download/v1.0.4/pluginval_Windows.zip'
$pluginvalZip = Join-Path $tools 'pluginval_Windows_v1.0.4.zip'
$pluginvalDir = Join-Path $tools 'pluginval-v1.0.4'
Invoke-WebRequest -Uri $pluginvalUrl -OutFile $pluginvalZip
Expand-Archive -LiteralPath $pluginvalZip -DestinationPath $pluginvalDir
$pluginval = Join-Path $pluginvalDir 'pluginval.exe'

$stage = [ordered]@{
    schema = 'profligacy-windows-acceptance-stage-v1'
    run_id = $RunId
    created_utc = [DateTime]::UtcNow.ToString('o')
    computer = $env:COMPUTERNAME
    os = (Get-CimInstance Win32_OperatingSystem).Caption
    architecture = $env:PROCESSOR_ARCHITECTURE
    artifacts = @(Get-ChildItem -LiteralPath $artifacts -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path = $_.FullName.Substring($artifacts.Length + 1)
            bytes = $_.Length
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash
        }
    })
    tools = @([ordered]@{
        name = 'pluginval'
        version = '1.0.4'
        source_url = $pluginvalUrl
        archive_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $pluginvalZip).Hash
    })
}
$stage | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 (Join-Path $receipts 'stage.json')

function Invoke-LoggedProcess(
    [string]$File,
    [string[]]$Arguments,
    [string]$Name
) {
    $stdout = Join-Path $logs "$Name.stdout.log"
    $stderr = Join-Path $logs "$Name.stderr.log"
    $process = Start-Process -FilePath $File -ArgumentList $Arguments -NoNewWindow `
        -Wait -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    return [ordered]@{
        exit_code = $process.ExitCode
        stdout = "logs\$Name.stdout.log"
        stdout_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $stdout).Hash
        stderr = "logs\$Name.stderr.log"
        stderr_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $stderr).Hash
    }
}

$env:PROPHECY_FORCE_NO_ROM = '1'
$env:PROPHECY_ROMPATH = $null
$env:PROPHECY_NVRAM = $null
$pluginvalResult = Invoke-LoggedProcess $pluginval @(
    '--strictness-level', '10', '--skip-gui-tests', '--timeout-ms', '900000',
    '--validate', $stagedVst3
) 'pluginval-headless'
$pluginvalResult.success_marker = [bool](Select-String -Quiet -SimpleMatch 'SUCCESS' `
    -LiteralPath (Join-Path $logs 'pluginval-headless.stdout.log'))

$validatorResult = $null
if ($SteinbergValidator) {
    $validatorResult = Invoke-LoggedProcess $SteinbergValidator @($stagedVst3) 'steinberg-validator'
    $validatorResult.success_marker = [bool](Select-String -Quiet -SimpleMatch '0 tests failed' `
        -LiteralPath (Join-Path $logs 'steinberg-validator.stdout.log'))
    $validatorResult.tool_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $SteinbergValidator).Hash
}

$noRomReceipt = Join-Path $receipts 'artifact-host-no-rom.json'
$noRomWav = Join-Path $logs 'artifact-host-no-rom.wav'
$noRomResult = Invoke-LoggedProcess $artifactHost @(
    '--plugin', $stagedVst3, '--receipt', $noRomReceipt, '--wav', $noRomWav,
    '--seconds', '2'
) 'artifact-host-no-rom'
$noRomBody = Get-Content -Raw -LiteralPath $noRomReceipt | ConvertFrom-Json

$romResult = $null
$romBody = $null
if ($RomRoot) {
    if (-not $NvramRoot) { throw '-NvramRoot is required with -RomRoot' }
    $env:PROPHECY_FORCE_NO_ROM = $null
    $env:PROPHECY_ROMPATH = $RomRoot
    $env:PROPHECY_NVRAM = $NvramRoot
    $env:KPROP_DSP_PERFRAME = '4'
    New-Item -ItemType Directory -Path $NvramRoot -Force | Out-Null
    $romReceipt = Join-Path $receipts 'artifact-host-rom.json'
    $romWav = Join-Path $logs 'artifact-host-rom.wav'
    $romResult = Invoke-LoggedProcess $artifactHost @(
        '--plugin', $stagedVst3, '--receipt', $romReceipt, '--wav', $romWav,
        '--seconds', '24', '--require-audio', '--realtime'
    ) 'artifact-host-rom'
    $romBody = Get-Content -Raw -LiteralPath $romReceipt | ConvertFrom-Json
}

$validatorSuccess = ($pluginvalResult.exit_code -eq 0) -and $pluginvalResult.success_marker -and
    (($null -eq $validatorResult) -or (($validatorResult.exit_code -eq 0) -and $validatorResult.success_marker))
$hostSuccess = ($noRomResult.exit_code -eq 0) -and $noRomBody.success -and
    (($null -eq $romResult) -or (($romResult.exit_code -eq 0) -and $romBody.success))
$summary = [ordered]@{
    schema = 'profligacy-windows-artifact-acceptance-v1'
    run_id = $RunId
    created_utc = [DateTime]::UtcNow.ToString('o')
    artifact_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $vst3Binary).Hash
    standalone_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $stagedStandalone).Hash
    artifact_host_sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $artifactHost).Hash
    pluginval = $pluginvalResult
    steinberg_validator = $validatorResult
    no_rom_host = $noRomResult
    private_rom_host = $romResult
    private_rom_audio = if ($null -eq $romBody) { $null } else { [ordered]@{
        nonzero_samples = $romBody.nonzero_samples; peak = $romBody.peak; rms = $romBody.rms
    }}
    success = [bool]($validatorSuccess -and $hostSuccess)
}
$summary | ConvertTo-Json -Depth 10 | Set-Content -Encoding UTF8 (Join-Path $receipts 'acceptance.json')
$summary | ConvertTo-Json -Depth 10
if (-not $summary.success) { exit 1 }
