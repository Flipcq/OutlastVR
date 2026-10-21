@echo off
setlocal
REM Builds Outlast1VR.exe from Outlast1VR_Launcher.cs + Outlast1VR_Pages.cs with the .NET Framework 4.x C# compiler.
cd /d "%~dp0"

set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (
  echo csc.exe from .NET Framework 4.x was not found.
  exit /b 1
)

set "EXTRA="
if exist "Outlast1VR.ico" set "EXTRA=%EXTRA% /win32icon:Outlast1VR.ico"
REM The logo is also embedded as a fallback; a strikezone_logo.jpg next to the exe wins at runtime.
if exist "strikezone_logo.jpg" set "EXTRA=%EXTRA% /resource:strikezone_logo.jpg,Outlast1VR.strikezone_logo.jpg"

"%CSC%" /nologo /target:winexe /optimize+ /platform:anycpu /out:Outlast1VR.exe %EXTRA% /codepage:65001 /reference:System.Windows.Forms.dll /reference:System.Drawing.dll Outlast1VR_Launcher.cs Outlast1VR_Pages.cs
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
echo Built "%~dp0Outlast1VR.exe"
exit /b 0
