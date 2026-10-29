$ErrorActionPreference = "Stop"
& "$PSScriptRoot/build-windows.ps1"
if ($LASTEXITCODE -ne 0) { throw "Build failed." }
$rootDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$classpath = "$rootDir/build/classes;$rootDir/build/test-classes"
foreach ($testName in @("RealtimeSmokeTest", "CameraApiSmokeTest")) {
    & "$env:JAVA_HOME/bin/java.exe" -XX:+UseZGC -Xms128m -Xmx128m `
      "-Djava.library.path=$rootDir/build/native" -cp $classpath "alcybe.realtime.$testName"
    if ($LASTEXITCODE -ne 0) { throw "$testName failed." }
}
foreach ($enabled in @("true", "false")) {
    & "$env:JAVA_HOME/bin/java.exe" -XX:+UseZGC -Xms128m -Xmx128m `
      "-Dalcybe.realtime.explicitGcOnIdle=$enabled" `
      "-Djava.library.path=$rootDir/build/native" -cp $classpath alcybe.realtime.RealtimeGcIdleTest
    if ($LASTEXITCODE -ne 0) { throw "RealtimeGcIdleTest failed." }
}
