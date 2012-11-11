@ echo off

set mediafolder=c:\windows\media

if "%~1" == "" (
    :: print sound names
    for %%a in (%mediafolder%\*.wav) do (
        echo %%~na
        )
    goto eof
    )

set file_sound=%mediafolder%\%~1.wav

if exist "%file_sound%" (
    start sndrec32 /play /close /embedding "%file_sound%"
    ) else (
    echo file not found: "%file_sound%" >&2
    )

:eof
exit /b

