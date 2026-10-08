@echo off
rem Builds the PS5 native application (Mupen64Plus PS5, PPSA99064) through WSL (Ubuntu-24.04).
rem   build-native.bat           app folder + zip in build-native\
rem   build-native.bat Ffpfsc    also a compressed .ffpfsc image
rem   build-native.bat Clean     from scratch (deletes ps5\build first); release.bat always does this
rem   build-native.bat Cpu       without the GPU renderer (angrylion only), in build-native-cpu\ (ps5\Makefile, VULKAN)
rem See ps5\README.md (Building) for the WSL setup.
setlocal
set "N64_DIR=%~dp0"
for /f "usebackq delims=" %%P in (`wsl.exe -d Ubuntu-24.04 -u root -e wslpath -a "%N64_DIR%."`) do set "N64_WSL=%%P"
wsl.exe -d Ubuntu-24.04 -u root -e bash "%N64_WSL%/ps5/tools/build-native.sh" %*
