/*
 * modes - list the display modes Windows offers on the primary display, or switch to one.
 * Usage: modes              list unique WxH modes (32 bpp) and the current mode
 *        modes W H          switch to WxH (not persisted to the registry), print the result
 * Run in the interactive session (session 0 sees no display).
 *
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char **argv)
{
    DEVMODEA dm = { 0 };
    dm.dmSize = sizeof(dm);
    if (argc == 3) {
        dm.dmPelsWidth = (DWORD)atoi(argv[1]);
        dm.dmPelsHeight = (DWORD)atoi(argv[2]);
        dm.dmBitsPerPel = 32;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
        LONG r = ChangeDisplaySettingsExA(NULL, &dm, NULL, 0, NULL);
        printf("set %lux%lu -> %ld (%s)\n", dm.dmPelsWidth, dm.dmPelsHeight, r,
               r == DISP_CHANGE_SUCCESSFUL ? "ok" : r == DISP_CHANGE_BADMODE ? "BADMODE" : "failed");
        Sleep(3000);
    } else {
        DWORD lastW = 0, lastH = 0;
        for (DWORD i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++) {
            if (dm.dmBitsPerPel != 32 || (dm.dmPelsWidth == lastW && dm.dmPelsHeight == lastH))
                continue;
            printf("mode %lux%lu @%lu\n", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
            lastW = dm.dmPelsWidth;
            lastH = dm.dmPelsHeight;
        }
    }
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm))
        printf("current %lux%lu\n", dm.dmPelsWidth, dm.dmPelsHeight);
    return 0;
}
