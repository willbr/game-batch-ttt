@ echo off

:: chimes
:: chord
:: ding
:: recycle
:: start
:: tada
:: windows xp battery critical
:: windows xp battery low
:: windows xp default
:: windows xp error
:: windows xp exclamation
:: windows xp hardware fail
:: windows xp hardware insert
:: windows xp hardware remove

set mediafolder=c:\windows\media

if "%~1" == "/list" (
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

