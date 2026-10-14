@echo off
REM SPDX-License-Identifier: GPL-2.0-or-later
REM Build ftpoc.exe (32-bit!). See ftpoc.c header for why 32-bit.
REM   i686-w64-mingw32-gcc -std=c99 -O2 -Wall -Wextra -static-libgcc -I..\..\libft -o ftpoc.exe ftpoc.c <libft\*.c, listed below>
REM -static-libgcc keeps the binary self-contained (no libgcc dll needed).
REM LIBFT: path to the repo's libft directory (default ..\..\libft).
if not defined LIBFT set LIBFT=..\..\libft
where cl >nul 2>nul
if %errorlevel%==0 (
    cl /O2 /I%LIBFT% ftpoc.c %LIBFT%\ft.c %LIBFT%\ft_geom.c %LIBFT%\ft_holder.c %LIBFT%\ft_focus.c %LIBFT%\ft_calib.c %LIBFT%\ft_job.c %LIBFT%\ft_png.c %LIBFT%\ft_detect.c %LIBFT%\ft_preview.c %LIBFT%\ft_plan.c %LIBFT%\ft_maint.c %LIBFT%\ft_upload.c %LIBFT%\ft_share.c %LIBFT%\ft_share_cut.c %LIBFT%\ft_develop.c %LIBFT%\ft_image.c
) else (
    i686-w64-mingw32-gcc -std=c99 -O2 -Wall -Wextra -static-libgcc -I%LIBFT% -o ftpoc.exe ftpoc.c %LIBFT%\ft.c %LIBFT%\ft_geom.c %LIBFT%\ft_holder.c %LIBFT%\ft_focus.c %LIBFT%\ft_calib.c %LIBFT%\ft_job.c %LIBFT%\ft_png.c %LIBFT%\ft_detect.c %LIBFT%\ft_preview.c %LIBFT%\ft_plan.c %LIBFT%\ft_maint.c %LIBFT%\ft_upload.c %LIBFT%\ft_share.c %LIBFT%\ft_share_cut.c %LIBFT%\ft_develop.c %LIBFT%\ft_image.c
)
