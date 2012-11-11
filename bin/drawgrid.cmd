:: drawgrid x__xoox_o

set arg1=%~1

set a=%arg1:~0,1%
set b=%arg1:~1,1%
set c=%arg1:~2,1%
set d=%arg1:~3,1%
set e=%arg1:~4,1%
set f=%arg1:~5,1%
set g=%arg1:~6,1%
set h=%arg1:~7,1%
set i=%arg1:~8,1%

set margin=  
echo.
echo %margin%%a% ^| %b% ^| %c%
echo %margin%---------
echo %margin%%d% ^| %e% ^| %f%
echo %margin%---------
echo %margin%%g% ^| %h% ^| %i%
echo.

