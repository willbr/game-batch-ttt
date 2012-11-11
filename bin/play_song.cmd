@echo off

if not exist "%~1" (
    echo song not found: "%~1" >&2
    exit /b
    )

for /f "usebackq tokens=1,2" %%a in ("%~1") do (
    if "%%a" == "s" (
        call sleep %%b
        ) else (
        call play_sound %%a
        )
    )

