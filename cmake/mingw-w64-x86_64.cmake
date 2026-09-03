# Cross-compile a Windows x64 DLL from Linux with mingw-w64.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(_prefix x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${_prefix}-gcc)
set(CMAKE_CXX_COMPILER ${_prefix}-g++)
set(CMAKE_RC_COMPILER  ${_prefix}-windres)

set(CMAKE_FIND_ROOT_PATH /usr/${_prefix})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
