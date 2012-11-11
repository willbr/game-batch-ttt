@echo off

echo ESC or ^C to quit

:loop
    reply
    echo %errorlevel%
    ::ESC
    if "%ERRORLEVEL%" EQU "27" goto exit
    ::^C
    if "%ERRORLEVEL%" EQU "0" goto exit
goto loop

:exit

