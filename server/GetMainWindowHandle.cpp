//==================================================================================================
// Find the Main HWND for a PID

//==================================================================================================
#include "GetMainWindowHandle.h"

#if defined(_WIN32)

struct handle_data
{
    unsigned long process_id;
    HWND window_handle;
};

bool is_main_window( HWND handle )
{
    return ( GetWindow( handle, GW_OWNER ) == (HWND)0 ) && IsWindowVisible( handle );
}

BOOL enum_windows_callback( HWND handle, LPARAM lParam )
{
    handle_data& data = *(handle_data*)lParam;
    unsigned long process_id = 0;
    GetWindowThreadProcessId( handle, &process_id );
    if (data.process_id != process_id || !is_main_window( handle ))
        return TRUE;
    data.window_handle = handle;
    return FALSE;
}


HWND find_main_window( unsigned long process_id )
{
    handle_data data;
    data.process_id = process_id;
    data.window_handle = 0;
    EnumWindows( enum_windows_callback, (LPARAM)&data );
    return data.window_handle;
}

#endif // if defined(_WIN32)
