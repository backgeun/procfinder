/*
 * client.c - ProcFinder usermode client
 *
 * Clean console UI with a built-in list of Call of Duty process names.
 * Communicates with ProcFinder.sys via IOCTL to get PID + base address.
 *
 * Build: cl.exe client.c /W3 /Fe:client.exe  (no WDK needed)
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <conio.h>
#include "..\shared\shared.h"

// -------------------------------------------------------------------------
// Console colour helpers
// -------------------------------------------------------------------------
#define COL_RESET   7   // light grey  (default)
#define COL_TITLE   11  // light cyan
#define COL_ITEM    15  // white
#define COL_SELECT  10  // light green
#define COL_DIM     8   // dark grey
#define COL_ERR     12  // light red
#define COL_OK      10  // light green
#define COL_LABEL   14  // yellow
#define COL_VALUE   15  // white

static void SetColor(int color)
{
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), (WORD)color);
}

static void ClearScreen(void)
{
    HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD count, written;
    COORD origin = { 0, 0 };

    GetConsoleScreenBufferInfo(hCon, &csbi);
    count = csbi.dwSize.X * csbi.dwSize.Y;
    FillConsoleOutputCharacterA(hCon, ' ', count, origin, &written);
    FillConsoleOutputAttribute(hCon, csbi.wAttributes, count, origin, &written);
    SetConsoleCursorPosition(hCon, origin);
}

static void SetCursor(int x, int y)
{
    COORD pos = { (SHORT)x, (SHORT)y };
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), pos);
}

static void HideCursor(void)
{
    CONSOLE_CURSOR_INFO ci = { 1, FALSE };
    SetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci);
}

// -------------------------------------------------------------------------
// Known Call of Duty process names
// -------------------------------------------------------------------------
typedef struct {
    const wchar_t *name;
    const wchar_t *label;
} CodEntry;

static const CodEntry COD_PROCESSES[] = {
    { L"blackops7.exe",       L"Black Ops 7"                  },
    { L"BlackOps6.exe",       L"Black Ops 6"                  },
    { L"BlackOps4.exe",       L"Black Ops 4 (Blackout)"       },
    { L"ModernWarfare3.exe",  L"Modern Warfare III (2023)"    },
    { L"ModernWarfare2.exe",  L"Modern Warfare II (2022)"     },
    { L"ModernWarfare.exe",   L"Modern Warfare (2019)"        },
    { L"Warzone.exe",         L"Warzone"                      },
    { L"cod.exe",             L"Call of Duty HQ (generic)"    },
    { L"notepad.exe",         L"[Test] Notepad"               },  // handy for testing
};

#define NUM_ENTRIES (int)(sizeof(COD_PROCESSES) / sizeof(COD_ENTRIES[0]))
// fix the macro below:
#define NUM_COD (int)(sizeof(COD_PROCESSES) / sizeof(COD_PROCESSES[0]))

// -------------------------------------------------------------------------
// UI constants
// -------------------------------------------------------------------------
#define BOX_X       2
#define BOX_Y       2
#define BOX_W       56
#define LIST_START_Y 8

// -------------------------------------------------------------------------
// Draw the static chrome (title box, labels)
// -------------------------------------------------------------------------
static void DrawChrome(void)
{
    ClearScreen();
    HideCursor();

    // Top border
    SetColor(COL_TITLE);
    SetCursor(BOX_X, BOX_Y);
    printf("\xC9");
    for (int i = 0; i < BOX_W - 2; i++) printf("\xCD");
    printf("\xBB");

    // Title row
    SetCursor(BOX_X, BOX_Y + 1);
    printf("\xBA");
    SetColor(COL_ITEM);
    int titleLen = 22;
    int pad = (BOX_W - 2 - titleLen) / 2;
    for (int i = 0; i < pad; i++) printf(" ");
    printf("  ProcFinder  v1.0  ");
    for (int i = 0; i < BOX_W - 2 - pad - titleLen; i++) printf(" ");
    SetColor(COL_TITLE);
    printf("\xBA");

    // Separator
    SetCursor(BOX_X, BOX_Y + 2);
    printf("\xC7");
    for (int i = 0; i < BOX_W - 2; i++) printf("\xC4");
    printf("\xB6");

    // Subtitle
    SetCursor(BOX_X, BOX_Y + 3);
    printf("\xBA");
    SetColor(COL_DIM);
    printf("  Select a Call of Duty process:");
    for (int i = 0; i < BOX_W - 2 - 33; i++) printf(" ");
    SetColor(COL_TITLE);
    printf("\xBA");

    // Separator
    SetCursor(BOX_X, BOX_Y + 4);
    printf("\xC7");
    for (int i = 0; i < BOX_W - 2; i++) printf("\xC4");
    printf("\xB6");

    // Bottom border will be drawn after the list
    int botY = LIST_START_Y + NUM_COD + 2;
    SetCursor(BOX_X, botY);
    SetColor(COL_TITLE);
    printf("\xC8");
    for (int i = 0; i < BOX_W - 2; i++) printf("\xCD");
    printf("\xBC");

    // Key hint
    SetCursor(BOX_X + 1, botY + 1);
    SetColor(COL_DIM);
    printf("[Up/Down] Navigate   [Enter] Select   [Q] Quit");
    SetColor(COL_RESET);
}

// -------------------------------------------------------------------------
// Draw the process list rows
// -------------------------------------------------------------------------
static void DrawList(int selected)
{
    for (int i = 0; i < NUM_COD; i++)
    {
        int row = LIST_START_Y + i;
        SetCursor(BOX_X, row);
        SetColor(COL_TITLE);
        printf("\xBA");

        if (i == selected)
        {
            SetColor(COL_SELECT);
            printf("  > %-*ls", BOX_W - 7, COD_PROCESSES[i].label);
        }
        else
        {
            SetColor(COL_DIM);
            printf("    ");
            SetColor(COL_ITEM);
            printf("%-*ls", BOX_W - 6, COD_PROCESSES[i].label);
        }

        SetColor(COL_TITLE);
        printf("\xBA");
    }

    // Draw separator below list
    int sepY = LIST_START_Y + NUM_COD;
    SetCursor(BOX_X, sepY);
    SetColor(COL_TITLE);
    printf("\xC7");
    for (int i = 0; i < BOX_W - 2; i++) printf("\xC4");
    printf("\xB6");

    // Empty result row (will be filled after query)
    SetCursor(BOX_X, sepY + 1);
    printf("\xBA");
    SetColor(COL_DIM);
    printf("%-*s", BOX_W - 2, "  Press Enter to query...");
    SetColor(COL_TITLE);
    printf("\xBA");
}

// -------------------------------------------------------------------------
// Draw the result row
// -------------------------------------------------------------------------
static void DrawResult(const PROCFINDER_RESPONSE *resp, const wchar_t *name)
{
    int resultY = LIST_START_Y + NUM_COD + 1;
    SetCursor(BOX_X, resultY);
    SetColor(COL_TITLE);
    printf("\xBA");

    if (resp == NULL)
    {
        // Driver not open
        SetColor(COL_ERR);
        printf("  %-*s", BOX_W - 3, "Driver not loaded! Run install.ps1 as Admin.");
        SetColor(COL_TITLE);
        printf("\xBA");
        return;
    }

    if (!resp->Found)
    {
        SetColor(COL_ERR);
        char buf[BOX_W];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "  Process not found: %ls", name);
        printf("%-*s", BOX_W - 2, buf);
        SetColor(COL_TITLE);
        printf("\xBA");
        return;
    }

    // Found — show PID and base address
    SetColor(COL_LABEL);
    printf("  PID: ");
    SetColor(COL_VALUE);
    printf("%-8lu  ", resp->ProcessId);
    SetColor(COL_LABEL);
    printf("Base: ");
    SetColor(COL_VALUE);
    printf("0x%016llX  ", resp->BaseAddress);
    // pad to fill box width
    int used = 6 + 10 + 8 + 22;
    for (int i = used; i < BOX_W - 2; i++) printf(" ");
    SetColor(COL_TITLE);
    printf("\xBA");
}

// -------------------------------------------------------------------------
// Query the driver
// -------------------------------------------------------------------------
static BOOL QueryDriver(const wchar_t *procName, PROCFINDER_RESPONSE *outResp)
{
    HANDLE hDev = CreateFileW(
        PROCFINDER_WIN32_DEVICE,
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);

    if (hDev == INVALID_HANDLE_VALUE)
        return FALSE;

    PROCFINDER_REQUEST req = { 0 };
    wcsncpy_s(req.ProcessName, PROC_NAME_MAX_LEN, procName, _TRUNCATE);

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        hDev,
        IOCTL_PROCFINDER_GET_INFO,
        &req,     sizeof(req),
        outResp,  sizeof(*outResp),
        &bytesReturned, NULL);

    CloseHandle(hDev);
    return ok;
}

// -------------------------------------------------------------------------
// Main
// -------------------------------------------------------------------------
int main(void)
{
    // Set console title and resize window for a clean look
    SetConsoleTitleW(L"ProcFinder");
    HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    SMALL_RECT rect = { 0, 0, 64, (SHORT)(LIST_START_Y + NUM_COD + 8) };
    SetConsoleWindowInfo(hCon, TRUE, &rect);
    COORD bufSize = { 65, (SHORT)(LIST_START_Y + NUM_COD + 10) };
    SetConsoleScreenBufferSize(hCon, bufSize);

    int selected = 0;
    BOOL hasResult = FALSE;
    PROCFINDER_RESPONSE lastResp = { 0 };
    BOOL driverMissing = FALSE;

    DrawChrome();
    DrawList(selected);

    while (1)
    {
        int ch = _getch();

        // Arrow keys come as 0xE0 followed by the code
        if (ch == 0xE0 || ch == 0)
        {
            ch = _getch();
            if (ch == 72) // up
            {
                selected = (selected - 1 + NUM_COD) % NUM_COD;
                DrawList(selected);
            }
            else if (ch == 80) // down
            {
                selected = (selected + 1) % NUM_COD;
                DrawList(selected);
            }
        }
        else if (ch == '\r' || ch == '\n') // Enter
        {
            const wchar_t *name = COD_PROCESSES[selected].name;
            PROCFINDER_RESPONSE resp = { 0 };

            if (!QueryDriver(name, &resp))
            {
                driverMissing = TRUE;
                DrawResult(NULL, name);
            }
            else
            {
                driverMissing = FALSE;
                lastResp = resp;
                hasResult = TRUE;
                DrawResult(&resp, name);
            }
        }
        else if (ch == 'q' || ch == 'Q')
        {
            break;
        }
    }

    // Restore and exit cleanly
    SetColor(COL_RESET);
    ClearScreen();
    SetCursor(0, 0);
    SetColor(COL_RESET);
    CONSOLE_CURSOR_INFO ci = { 10, TRUE };
    SetConsoleCursorInfo(hCon, &ci);
    return 0;
}
