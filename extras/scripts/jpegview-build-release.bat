@echo off

setlocal

REM Maintainer release-packaging script for JPEGView.
REM Builds Release x86 and x64 with CMake + Visual Studio 2022 and zips up the
REM outputs.  The old VS2017/XP edition and the per-vcxproj bin/ layout have
REM been removed; this script now drives the CMake build instead.
REM /sylikc (original), CMake port

set JPV_ROOT=%~dp0..\..
set XOUT_BASE=%~dp0jpegview-release
set XRAW_OUT=%XOUT_BASE%\raw_out

REM so we don't end up building multiple releases together accidentally
IF EXIST "%XOUT_BASE%" (
	echo OUTPUT directory already exists, please delete and try again
	exit /b 1
)

REM detect version from source
echo ~ Resource:
findstr /l /c:JPEGVIEW_VERSION "%JPV_ROOT%\src\JPEGView\resource.h"
echo ~ rc:
findstr /l /i /c:FILEVERSION /c:PRODUCTVERSION "%JPV_ROOT%\src\JPEGView\JPEGView.rc"

SET JPV_VER=%~1
echo ~ Command Line: %JPV_VER%

if /I "%JPV_VER%" EQU "" (
	echo Argument must be target release version e.g. 1.0.34
	exit /b 1
)

SET YESNO=
SET /P YESNO="Have you made all the final code edits? y/[n] "
IF /I "%YESNO%" NEQ "y" (
	echo Aborted!
	exit /b 1
)

REM detect tools
SET X7ZA=C:\Program Files\7-Zip\7z.exe
IF NOT EXIST "%X7ZA%" (
	echo ERROR: 7-zip not found in standard location
	exit /b 1
)

echo + Looking up cmake
where cmake.exe
IF ERRORLEVEL 1 (
	echo ERROR: cmake.exe not found on path
	exit /b 1
)

echo + Creating %XOUT_BASE% ...
mkdir "%XOUT_BASE%"
mkdir "%XRAW_OUT%"

echo ------------------------------------------------------
echo Creating release %JPV_VER% in folder %XOUT_BASE%
echo ------------------------------------------------------

echo + Cleaning previous CMake build dirs ...
rd /s /q "%JPV_ROOT%\build"      2>nul
rd /s /q "%JPV_ROOT%\build32"    2>nul

call :BUILD_COPY_JPV x64 x64 64
IF ERRORLEVEL 1 exit /b 1
call :BUILD_COPY_JPV x86 Win32 32
IF ERRORLEVEL 1 exit /b 1

REM Copy docs to OUTPUT
copy "%JPV_ROOT%\HowToInstall.txt" "%XRAW_OUT%"
copy "%JPV_ROOT%\HowToInstall_ru.txt" "%XRAW_OUT%"
copy "%JPV_ROOT%\CHANGELOG.txt" "%XRAW_OUT%"

REM remove intermediates
del /s "%XRAW_OUT%\*.exp" "%XRAW_OUT%\*.lib" "%XRAW_OUT%\*.pdb"

REM ZIP up flat from RAW OUTPUT
pushd "%XRAW_OUT%"
"%X7ZA%" a -tzip -mx9 ..\JPEGView_%JPV_VER%.zip .
IF ERRORLEVEL 1 exit /b 1
"%X7ZA%" a -t7z -mx9 ..\JPEGView_%JPV_VER%.7z .
IF ERRORLEVEL 1 exit /b 1

pushd JPEGView64
"%X7ZA%" a -t7z -mx9 ..\..\JPEGView64_%JPV_VER%.7z .
IF ERRORLEVEL 1 exit /b 1
popd

pushd JPEGView32
"%X7ZA%" a -t7z -mx9 ..\..\JPEGView32_%JPV_VER%.7z .
IF ERRORLEVEL 1 exit /b 1
popd
popd

REM generate SHA256SUMS
pushd "%XOUT_BASE%"
python.exe "%~dp0sha256sum.py" "JPEGView*" > SHA256SUMS
IF ERRORLEVEL 1 exit /b 1
popd

echo + Autogenerate last chunk of changelog ...
set JPV_ROOT=..\..
python.exe -c "from pathlib import Path; from util_common import get_all_text_between; print(get_all_text_between(Path(r'%JPV_ROOT%\CHANGELOG.txt'), '\[%JPV_VER%', '\['))"

exit /b 0


REM -----------------------------------------------------------------------------------------------------------

:BUILD_COPY_JPV

setlocal

SET XARCH=%~1
SET XPLATFORM=%~2
SET XBIT=%~3

REM Configure + build with CMake.  JPEGVIEW_BUILD_SETUP=ON also produces the MSI.
cmake -B "%JPV_ROOT%\build%XARCH%" -S "%JPV_ROOT%" -G "Visual Studio 17 2022" -A %XPLATFORM% -DJPEGVIEW_BUILD_SETUP=ON
IF ERRORLEVEL 1 exit /b 1

cmake --build "%JPV_ROOT%\build%XARCH%" --config Release --target JPEGView.Setup
IF ERRORLEVEL 1 exit /b 1

REM from build/out/Release to OUTPUT\JPEGViewNN
move "%JPV_ROOT%\build%XARCH%\out\Release" "%XRAW_OUT%\JPEGView%XBIT%"
IF ERRORLEVEL 1 exit /b 1

REM move the wix installer
move "%JPV_ROOT%\build%XARCH%\msi\JPEGView-%XPLATFORM%-Release.msi" "%XOUT_BASE%\JPEGView%XBIT%_en-us_%JPV_VER%.msi"
IF ERRORLEVEL 1 exit /b 1

exit /b 0
