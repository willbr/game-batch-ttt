@echo off

rem TODO add perfect tictactoe ai

setlocal EnableDelayedExpansion
if errorlevel 1 (
    echo failed to enable delayed expasion
    exit /b
)

set game=qweasdzxc
set logging=false

set player_char=X
set computer_char=O

if "%~1"=="/test" goto :tests


echo Keys:

call :draw

echo Ready...
call sleep 500
echo Go!

call :reset_game
call :setup_input
call :game_loop

if "%winner%" NEQ "" call play_sound tada

echo winner: %winner%

pause

exit /b

:draw none
    call drawgrid %game%
    exit /b

:reset_game none
    set winner=
    set game_running=true
    set game=---------
    set turn=0
    exit /b

:game_loop none

    call :log looping
    call :log turn: %turn%
    call :log game: %game%

    call :think
    call :check_for_game_over
    if errorlevel 1 exit /b
    call :draw

    call :parse_input
    call :check_for_game_over
    if errorlevel 1 exit /b
    call :draw

    if "%game_running%" EQU "true" goto game_loop
    exit /b

:parse_input none
    if "%game_running%" NEQ "true" exit /b
    rem get key
    reply
    set key=%errorlevel%
    rem lookup and execute key mapping
    call set input_command=%%key_%key%%%
    call %input_command%
    exit /b
    

:setup_input none
    set key_0=:end_game
    set key_27=:end_game

    set key_113=:move_q
    set key_81=:move_q
    set key_119=:move_w
    set key_87=:move_w
    set key_101=:move_e
    set key_69=:move_e

    set key_97=:move_a
    set key_65=:move_a
    set key_115=:move_s
    set key_83=:move_s
    set key_100=:move_d
    set key_68=:move_d

    set key_122=:move_z
    set key_90=:move_z
    set key_120=:move_x
    set key_88=:move_x
    set key_99=:move_c
    set key_67=:move_c
    exit /b

:move_q
    call :set_cell 0 %player_char%
    exit /b

:move_w
    call :set_cell 1 %player_char%
    exit /b

:move_e
    call :set_cell 2 %player_char%
    exit /b

:move_a
    call :set_cell 3 %player_char%
    exit /b

:move_s
    call :set_cell 4 %player_char%
    exit /b

:move_d
    call :set_cell 5 %player_char%
    exit /b

:move_z
    call :set_cell 6 %player_char%
    exit /b

:move_x
    call :set_cell 7 %player_char%
    exit /b

:move_c
    call :set_cell 8 %player_char%
    exit /b

:end_game
    set game_running=false
    exit /b

:get_cell return_value cell_number
    call set %1=%%game:~%2,1%%
    exit /b

:free_cell cell_number
    call :get_cell cell %1
    if "%cell%"=="-" (
        exit /b 0
    ) else (
        exit /b 1
    )

:set_cell cell_number char
    call :log set_cell %1 %2
    set /a length= 8 - %1
    set /a offset= 1 + %1
    call :log length: %length%
    call :log game: %game%
    call :free_cell %1
    if errorlevel 1 (
        call :log cell isn't free
    ) else (
        call :log cell is free
        set /a turn += 1
        if %1 EQU 0 (
            call set left=
            call set right=%%game:~1,8%%
        ) else if %1 EQU 8 (
            call set left=%%game:~0,8%%
            call set right=
        ) else (
            call set left=%%game:~0,%1%%
            call set right=%%game:~%offset%,%length%%%
        )
        call set game=%%left%%%2%%right%%
    )
    call :log left: %left%
    call :log right: %right%
    call :log game: %game%
    exit /b

:log *message
    if "%logging%" == "true" (
        echo %* >&2
    )
    exit /b

:think
    set /a computers_turn=%turn% %% 2
    if %computers_turn% EQU 1 (
        echo thinking
        call :ai_make_perfect_move
    )
    exit /b

:ai_make_move
    set start_turn=%turn%
    set ai_think=0
    :ai_loop
    set /a ai_think+=1
    set /a cell_choice=(%RANDOM% * 9) / 32767
    call :set_cell %cell_choice% %computer_char%
    if %ai_think% GTR 100 exit /b
    if %start_turn% EQU %turn% goto :ai_loop
    exit /b

:ai_make_perfect_move
    call :make_move_winning
    if errorlevel 1 (
        exit /b
    )
    call :make_move_block
    if errorlevel 1 (
        exit /b
    )
    call :make_move_fork
    if errorlevel 1 (
        exit /b
    )
    call :make_move_block_fork
    if errorlevel 1 (
        exit /b
    )
    call :make_move_center
    if errorlevel 1 (
        exit /b
    )
    call :make_move_opposite_corner_to_opponent
    if errorlevel 1 (
        exit /b
    )
    call :make_move_empty_corner
    if errorlevel 1 (
        exit /b
    )
    call :make_move_empty_side
    if errorlevel 1 (
        exit /b
    )
    exit /b

:make_move_winning
    echo move
    exit /b

:make_move_block
    echo move
    exit /b

:make_move_fork
    echo move
    exit /b

:make_move_block_fork
    echo move
    exit /b

:make_move_center
    echo move
    exit /b

:make_move_opposite_corner_to_opponent
    call :log game: %game%
    for %%a in (0 2 6 8) do (
        call :get_cell cell %%a
        call :get_opposite_cell opposite %%a
        if "!cell!"=="!player_char!" (
            call :log found player cell
            call :free_cell !opposite!
            if errorlevel 1 (
                call :log opposite isn't free
            ) else (
                call :set_cell !opposite! !computer_char!
                exit /b 1
            )
        ) else (
            call :log cell %%a : !cell! ^!= !player_char!
        )
    )
    exit /b 0

:get_opposite_cell return_value cell_number
    if %2 EQU 0 ((set %1=8) && exit /b 0)
    if %2 EQU 2 ((set %1=6) && exit /b 0)
    if %2 EQU 6 ((set %1=2) && exit /b 0)
    if %2 EQU 8 ((set %1=0) && exit /b 0)
    exit /b 1

:make_move_empty_corner
    call :get_empty_cell cell "0 2 6 8"
    if errorlevel 1 (
        echo no empty cell
    ) else (
        call :set_cell %cell% %computer_char%
        exit /b 1
    )
    exit /b 0

:make_move_empty_side
    call :get_empty_cell cell "1 3 5 7"
    if errorlevel 1 (
        echo no empty cell
    ) else (
        call :set_cell %cell% %computer_char%
        exit /b 1
    )
    exit /b 0

:get_empty_cell return_value cells
    for %%a in (%~2) do (
        call :log cell %%a
        call :free_cell %%a
        if errorlevel 1 (
            call :log skipping filled cell: %%a
        ) else (
            set %1=%%a
            exit /b 0
        )
    )
    exit /b 1

:check_for_game_over
    call :find_line %player_char%
    if errorlevel 1 (
        set winner=player
        set game_running=false
        )
    call :find_line %computer_char%
    if errorlevel 1 (
        set winner=computer
        set game_running=false
    )
    call :check_if_board_is_full
    if errorlevel 1 (
        set winner=
        set game_running=false
    )
    exit /b

:check_if_board_is_full
    for /l %%a in (0,1,8) do (
        call :free_cell %%a
        if errorlevel 1 (
            rem skip
        ) else (
            call :log found free cell
            exit /b 0
        )
    )
    exit /b 1

:find_line char
    set match_line=%1%1%1

    call :log match_line: %match_line%
    rem rows
    if "%match_line%"=="%game:~0,3%" exit /b 1
    if "%match_line%"=="%game:~3,3%" exit /b 1
    if "%match_line%"=="%game:~6,3%" exit /b 1
    call :log row1: %game:~0,3%
    call :log row2: %game:~3,3%
    call :log row3: %game:~6,3%

    rem cols
    if "%match_line%"=="%game:~0,1%%game:~3,1%%game:~6,1%" exit /b 1
    if "%match_line%"=="%game:~1,1%%game:~4,1%%game:~7,1%" exit /b 1
    if "%match_line%"=="%game:~2,1%%game:~5,1%%game:~8,1%" exit /b 1
    call :log col1: %game:~0,1%%game:~3,1%%game:~6,1%
    call :log col2: %game:~1,1%%game:~4,1%%game:~7,1%
    call :log col3: %game:~2,1%%game:~5,1%%game:~8,1%

    rem diag
    if "%match_line%"=="%game:~0,1%%game:~4,1%%game:~8,1%" exit /b 1
    if "%match_line%"=="%game:~2,1%%game:~4,1%%game:~6,1%" exit /b 1
    call :log diag1: %game:~0,1%%game:~4,1%%game:~8,1%
    call :log diag2: %game:~2,1%%game:~4,1%%game:~6,1%
    exit /b 0

rem ==============================
rem TESTS
rem ==============================

:tests
    set logging=true
    echo Unit Tests
    echo.
    set pass=0
    set fail=0
    for /f %%a in (%0) do (
        set line=%%a
        if "!line:~0,6!"==":test_" (
            set current_test=%%a
            call %%a > tmp\test_output.txt 2>&1
            if "!last_test!"=="fail" (
                echo %%a
                type tmp\test_output.txt
                echo.
                )
            )
        )

    set /a total=%pass% + %fail%
    echo.
    echo pass: %pass%
    echo fail: %fail%
    echo.
    pause
    exit /b

:test_move_q
    set game=---------
    call :move_q
    call :assert_equal "%game%" "X--------"
    exit /b 0

:test_moves_ew
    set game=---------
    call :move_e
    call :move_w
    call :assert_equal "%game%" "-XX------"
    exit /b 0

:test_free_cell_success
    set game=---------
    call :free_cell 0
    call :assert_equal "%errorlevel%" "0"
    exit /b 0

:test_free_cell_fail
    set game=X--------
    call :free_cell 0
    call :assert_equal "%errorlevel%" "1"
    exit /b 0

:test_set_cell_pass
    set game=---------
    call :set_cell 0 X
    call :assert_equal "%game%" "X--------"
    exit /b 0

:test_find_line_none
    set game=---------
    call :find_line X
    call :assert_equal "%errorlevel%" "0"
    exit /b 0

:test_find_line_row1
    set game=XXX------
    call :find_line X
    call :assert_equal "%errorlevel%" "1"
    exit /b 0

:test_find_line_col1
    set game=X--X--X--
    call :find_line X
    call :assert_equal "%errorlevel%" "1"
    exit /b 0

:test_find_line_diag1
    set game=X---X---X
    call :find_line X
    call :assert_equal "%errorlevel%" "1"
    exit /b 0

:test_find_line_diag2
    set game=X-OXO-O-X
    call :find_line O
    call :assert_equal "%errorlevel%" "1"
    exit /b 0

:test_check_if_board_is_full_no
    set game=---------
    call :check_if_board_is_full
    call :assert_equal "%errorlevel%" "0"
    exit /b

:test_check_if_board_is_full_yes
    set game=XXXXXXXXX
    call :check_if_board_is_full
    call :assert_equal "%errorlevel%" "1"
    exit /b

:test_make_move_empty_side
    set game=---------
    call :make_move_empty_side
    call :assert_equal "%game%" "-O-------"
    exit /b

:test_make_move_empty_corner
    set game=---------
    call :make_move_empty_corner
    call :assert_equal "%game%" "O--------"
    exit /b

:test_make_move_opposite_corner_to_opponent
    set game=X--------
    call :make_move_opposite_corner_to_opponent
    call :assert_equal "%game%" "X-------O"
    exit /b

:test_make_move_center
    set game=---------
    call :make_move_center
    call :assert_equal "%game%" "----O----"
    exit /b

:test_get_cell
    set game=X--------
    call :get_cell cell 0
    call :assert_equal "%cell%" "X"

    set game=X--------
    call :get_cell cell 1
    call :assert_equal "%cell%" "-"

    set game=--------O
    call :get_cell cell 8
    call :assert_equal "%cell%" "O"
    exit /b

:test_get_opposite_cell
    call :get_opposite_cell cell 0
    call :assert_equal "%cell%" "8"
    call :get_opposite_cell cell 2
    call :assert_equal "%cell%" "6"
    call :get_opposite_cell cell 6
    call :assert_equal "%cell%" "2"
    call :get_opposite_cell cell 8
    call :assert_equal "%cell%" "0"
    exit /b

:assert_equal
    if "%~1" EQU "%~2" (
        set /a pass += 1
        set last_test=pass
        ) else (
        echo failed: %current_test% >&2
        echo %0 %* >&2
        echo. >&2
        set /a fail += 1
        set last_test=fail
        )
    exit /b

:eof
endlocal

