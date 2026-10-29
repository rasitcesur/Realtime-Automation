param(
    [string]$deviceId = "0",
    [int]$width = 640,
    [int]$height = 480,
    [int]$fps = 30,
    [int]$frameCount = 100
)
$ErrorActionPreference = "Stop"
& "$PSScriptRoot/build-windows.ps1"
if ($LASTEXITCODE -ne 0) { throw "Build failed." }
$rootDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$javaFlags = @("-XX:+UseZGC", "-XX:+ExplicitGCInvokesConcurrent")
$releaseFile = Join-Path $env:JAVA_HOME "release"
if (Test-Path $releaseFile) {
    $versionLine = Get-Content $releaseFile | Where-Object { $_ -match '^JAVA_VERSION=' } | Select-Object -First 1
    if ($versionLine -match '"(\d+)') {
        $javaMajor = [int]$Matches[1]
        if ($javaMajor -le 22) { $javaFlags += "-XX:+ZGenerational" }
    }
}
& "$env:JAVA_HOME/bin/java.exe" @javaFlags -Xms256m -Xmx2G `
  "-Djava.library.path=$rootDir/build/native" `
  -cp "$rootDir/build/classes" alcybe.realtime.CameraDemo `
  $deviceId $width $height $fps $frameCount "ARGB"
if ($LASTEXITCODE -ne 0) { throw "Camera demo failed." }
