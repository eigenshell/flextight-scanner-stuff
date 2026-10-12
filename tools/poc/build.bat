@echo off
REM SPDX-License-Identifier: GPL-2.0-or-later
REM Build ftpoc.exe (32-bit!). See ftpoc.c header for why 32-bit.
REM   i686-w64-mingw32-gcc -std=c99 -O2 -Wall -Wextra -static-libgcc -I..\..\libft -o ftpoc.exe ftpoc.c ..\..\libft\*.c
REM -static-libgcc keeps the binary self-contained (no libgcc dll needed).
REM LIBFT: path to the repo's libft directory (default ..\..\libft).
if not defined LIBFT set LIBFT=..\..\libft
where cl >nul 2>nul
if %errorlevel%==0 (
    cl /O2 /I%LIBFT% ftpoc.c %LIBFT%\*.c
) else (
    i686-w64-mingw32-gcc -std=c99 -O2 -Wall -Wextra -static-libgcc -I%LIBFT% -o ftpoc.exe ftpoc.c %LIBFT%\*.c
)
