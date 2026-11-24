@echo off
rem Runs the headless self-test. The work is in tools\test.ps1; this launcher keeps the entry point
rem consistent with build.bat and avoids remembering the fixtures/log arguments.
rem
rem   test.bat                         build unpacked, then run with build\fixtures
rem   test.bat -Fast -Suite search     fast non-LTCG test-only build + focused suite (fastest dev loop)
rem   test.bat -Headless              offline suites, no native UI or audio device
rem   test.bat -List                  list suites, prerequisites and source files
rem   test.bat -Suite search,sorting  focus a bug investigation
rem   test.bat -NoBuild -Changed      the suites covering uncommitted changes (tools\test-map.psd1)
rem   test.bat -Repeat 10 -Seed 42    reproduce intermittent and randomized failures
rem   test.bat -NoBuild               run the existing build\cordinal-test.exe
rem   test.bat -Fixtures D:\fixtures   use a fixture folder made earlier
rem   test.bat -Log D:\selftest.log    keep the log elsewhere
rem   test.bat -ShowPasses            also print each passing suite (the log always has them)
setlocal
cd /d "%~dp0"
set "PS=powershell.exe"
where pwsh.exe >nul 2>nul && set "PS=pwsh.exe"
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "tools\test.ps1" %*
exit /b %ERRORLEVEL%
