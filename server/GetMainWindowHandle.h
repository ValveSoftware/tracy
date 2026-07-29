#pragma once

#if defined(_WIN32)

#include <windows.h>
HWND find_main_window( unsigned long process_id );

#endif // if defined(_WIN32)
