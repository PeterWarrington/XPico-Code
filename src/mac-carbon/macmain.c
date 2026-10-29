/* Entry point for the Mac build.
 *
 * The app's startup lives in WinMain; Retro68's runtime calls main. Carbon
 * initialises most of the Toolbox itself, so this only has to set up the
 * cursor and the menu bar before handing over. */
#include <Carbon.h>
#include "win32.h"

int WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show);

int main(void)
{
    InitCursor();
    FlushEvents(everyEvent, 0);
    /* files dropped on the app, or opened from the Finder */
    mac_install_drop_handler();
    /* make the Finder read our BNDL, so the icon family is used */
    mac_bless_self();
    return WinMain(NULL, NULL, "", SW_SHOWNORMAL);
}
