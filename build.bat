@echo off
setlocal

REM Set up the build environment
set SRC_DIR=src
set OUT_DIR=build
set EXE_NAME=slate.exe

REM Create output directory if it doesn't exist
if not exist %OUT_DIR% mkdir %OUT_DIR%

REM Compile and link the application
cl /nologo /W4 /MD /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /I"%SRC_DIR%" ^
   /Fe"%OUT_DIR%\%EXE_NAME%" ^
   "%SRC_DIR%\main.c" "%SRC_DIR%\slate_doc.c" "%SRC_DIR%\slate_view.c" "%SRC_DIR%\slate.c" ^
   /link /SUBSYSTEM:WINDOWS ^
         user32.lib gdi32.lib comctl32.lib comdlg32.lib shell32.lib

if %ERRORLEVEL% EQU 0 (
    echo Build successful!
    echo Executable created: %OUT_DIR%\%EXE_NAME%
) else (
    echo Build failed!
)

endlocal