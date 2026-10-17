@echo off
REM SPDX-License-Identifier: GPL-2.0-or-later
REM Build ftwebd.exe with mingw (posix threads: winlibs "posix" builds).
REM -static links winpthreads in, so the exe needs no dll beside it.
REM LIBFT: path to the repo's libft directory (default ..\..\libft).
if not defined LIBFT set LIBFT=..\..\libft
gcc -std=c99 -O2 -Wall -Wextra -static -I%LIBFT% -o ftwebd.exe ftwebd.c http.c task.c proc_win.c %LIBFT%\ft.c %LIBFT%\ft_geom.c %LIBFT%\ft_holder.c %LIBFT%\ft_focus.c %LIBFT%\ft_focus_io.c %LIBFT%\ft_job.c %LIBFT%\ft_preview.c %LIBFT%\ft_share.c %LIBFT%\ft_plan.c -lws2_32 -lm
