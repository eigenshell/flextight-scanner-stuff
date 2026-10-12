@echo off
REM SPDX-License-Identifier: GPL-2.0-or-later
REM Build ftpoc.exe (32-bit!). See ftpoc.c header for why 32-bit.
REM   i686-w64-mingw32-gcc -O2 -Wall -Wextra -static-libgcc -o ftpoc.exe ftpoc.c
REM -static-libgcc keeps the binary self-contained (no libgcc dll needed).
where cl >nul 2>nul
if %errorlevel%==0 (
    cl /O2 ftpoc.c
) else (
    i686-w64-mingw32-gcc -O2 -Wall -Wextra -static-libgcc -o ftpoc.exe ftpoc.c
)
