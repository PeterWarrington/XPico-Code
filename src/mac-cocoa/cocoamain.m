/* Entry point for the Cocoa build.
 *
 * The app's startup lives in WinMain. This only has to bring NSApplication up
 * far enough for a window and a menu bar to work, and then hand over: the
 * editor runs its own Win32 message loop, so -[NSApplication run] is never
 * called and GetMessage pumps the events instead (see mac-cocoa/wnd.m).
 *
 * -finishLaunching is the half of -run that matters here: it registers for
 * Apple Events, tells the Dock the app is up and sends
 * applicationDidFinishLaunching. */
#import <Cocoa/Cocoa.h>
#include "win32.h"

int WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show);

int main(int argc, const char *argv[])
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    int rc;
    (void)argc; (void)argv;

    [NSApplication sharedApplication];
    [NSApp finishLaunching];
    /* Files opened from the Finder arrive through the application delegate,
     * which mac-cocoa/wnd.m installs with the window. */
    mac_install_drop_handler();

    rc = WinMain(NULL, NULL, "", SW_SHOWNORMAL);

    [pool release];
    return rc;
}
