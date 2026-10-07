@echo off
rem Publishes a Mupen64Plus PS5 release on GitHub (TheRealRetro/mupen64plus-ps5).
rem   release.bat              build, then publish build-native\PPSA99064.zip as release <contentVersion>
rem   release.bat -SkipBuild   publish what build-native\ already holds
rem Before: raise VERSION in ps5\Makefile, write release-notes\<contentVersion>.md, commit and push.
rem See ps5\tools\release.ps1.
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0ps5\tools\release.ps1" %*
