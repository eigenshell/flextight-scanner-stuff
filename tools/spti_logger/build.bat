@echo off
REM Build ft_hook.dll (32-bit!) and inject.exe for the FlexColor transport trace.
REM Requires Visual Studio (cl) in PATH, or mingw-w64 targeting i686:
REM   i686-w64-mingw32-gcc -shared -O2 -Wall -Wextra -o ft_hook.dll hook.c fmt.c
REM   i686-w64-mingw32-gcc -O2 -Wall -Wextra -o inject.exe inject.c
REM BOTH MUST BE 32-BIT: FlexColor.exe is a PE32 (i386) process; a 64-bit
REM ft_hook.dll cannot be loaded into it, and a 64-bit inject.exe cannot
REM create a remote thread in it.
cl /LD /O2 hook.c fmt.c /Fe:ft_hook.dll
cl /O2 inject.c
