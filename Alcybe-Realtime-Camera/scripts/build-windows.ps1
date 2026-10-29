$ErrorActionPreference = "Stop"
$rootDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $env:JAVA_HOME) { throw "JAVA_HOME must point to JDK 21 or later." }
$classesDir = Join-Path $rootDir "build/classes"
$testClassesDir = Join-Path $rootDir "build/test-classes"
New-Item -ItemType Directory -Force -Path $classesDir, $testClassesDir | Out-Null
$javaFiles = @(Get-ChildItem "$rootDir/src/main/java" -Filter "*.java" -Recurse | ForEach-Object FullName)
$testFiles = @(Get-ChildItem "$rootDir/src/test/java" -Filter "*.java" -Recurse | ForEach-Object FullName)
& "$env:JAVA_HOME/bin/javac.exe" -encoding UTF-8 -d $classesDir @javaFiles
if ($LASTEXITCODE -ne 0) { throw "Java compilation failed." }
& "$env:JAVA_HOME/bin/javac.exe" -encoding UTF-8 -cp $classesDir -d $testClassesDir @testFiles
if ($LASTEXITCODE -ne 0) { throw "Test compilation failed." }
& cmake -S $rootDir -B "$rootDir/build/cmake-windows" -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
& cmake --build "$rootDir/build/cmake-windows" --parallel
if ($LASTEXITCODE -ne 0) { throw "Native compilation failed." }
