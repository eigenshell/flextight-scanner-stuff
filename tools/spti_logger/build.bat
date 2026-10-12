@echo off
REM SPDX-License-Identifier: GPL-2.0-or-later
REM Build ft_hook.dll (32-bit!), inject.exe and uiev.exe for the FlexColor
REM transport trace + UI event timeline.
REM Requires Visual Studio (cl) in PATH, or mingw-w64 targeting i686:
REM   i686-w64-mingw32-gcc -shared -O2 -Wall -Wextra -static-libgcc -o ft_hook.dll hook.c fmt.c
REM   i686-w64-mingw32-gcc -O2 -Wall -Wextra -static-libgcc -o inject.exe inject.c
REM   i686-w64-mingw32-gcc -O2 -Wall -Wextra -static-libgcc -o uiev.exe uiev.c
REM BOTH MUST BE 32-BIT: FlexColor.exe is a PE32 (i386) process; a 64-bit
REM ft_hook.dll cannot be loaded into it, and a 64-bit inject.exe cannot
REM create a remote thread in it. (uiev.exe uses system-wide LL hooks and runs
REM in its own process, so its bitness is not constrained — we still build
REM 32-bit to keep one toolchain.)
REM -static-libgcc is required for the mingw build: without it the binaries
REM depend on libgcc_s_dw2-1.dll at runtime (inject won't launch; the hook
REM DLL would need it resolvable from inside FlexColor's process).
cl /LD /O2 hook.c fmt.c /Fe:ft_hook.dll
cl /O2 inject.c
cl /O2 uiev.c
