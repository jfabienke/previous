/*
  Previous - automation.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Scripted input for unattended runs. PREVIOUS_INPUT=FILE makes Previous
  follow FILE, which may be appended to while it runs, and drive the NeXT
  keyboard and mouse from the m68k thread in emulated time:

    wait MS              pause for MS milliseconds of emulated time
    move DX DY           move the mouse by DX, DY, one unit per event: slow
                         enough for NEXTSTEP not to accelerate it
    jump DX DY           move in events of up to 63 units (accelerated; for
                         reaching a screen corner)
    pace MS              milliseconds between mouse events (default 0.2);
                         NEXTSTEP accelerates fast movement, so slow it down
                         for exact positions
    down [right]         press, release or click a mouse button
    up [right]
    click [right]
    key NAME [MOD...]    press and release a key, NAME an SDL scancode name
                         (Return, Tab, A, Slash, ...), MOD cmd, shift, alt
                         or ctrl
    type TEXT            type TEXT (US layout)
    snap                 ask a board for a screen snapshot
    profile start        a NeXTdimension board starts a clock profile of its
                         i860 (Appendix C clocks per instruction address)
    profile write FILE   ... writes it to FILE and stops
    grab                 save the screen Previous shows (the Cube's own, in
                         single-screen mode) as next_screen_NNN.png in the
                         snapshot directory (PREVIOUS_ND_SNAPSHOT)
  Lines starting with # are comments.
*/

#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "configuration.h"
#include "log.h"
#include "kms.h"
#include "gui-sdl/sdlkeymap.h"
#include "automation.h"
#include "grab.h"

#define MOUSE_STEP_MS  0.2   /* between mouse events, by default */
#define KEY_STEP_MS    30    /* between key and button transitions */
#define POLL_MS        10    /* between looks at the file at its end */

enum { A_WAIT, A_MOVE, A_DOWN, A_UP, A_KEYDOWN, A_KEYUP, A_SNAP, A_GRAB, A_PROFILE };

typedef struct {
    int     kind;
    int     a, b;       /* move: dx, dy; button: right; key: modifiers, key;
                           profile: AUTOMATION_PROFILE_START or _WRITE */
    int64_t delay;      /* cycles after the action */
    char*   text;       /* profile write: the file (allocated) */
} Action;

volatile int Automation_SnapshotRequest;
volatile int Automation_ProfileRequest;
char         Automation_ProfilePath[FILENAME_MAX];

static FILE*   input;
static Action* queue;
static size_t  qhead, qlen, qcap;
static int64_t remaining;
static int64_t cyclesPerMs;
static double  mouseStepMs = MOUSE_STEP_MS;

static void push(int kind, int a, int b, double delay_ms) {
    if (qlen == qcap) {
        qcap = qcap ? 2 * qcap : 256;
        queue = (Action*)realloc(queue, qcap * sizeof(Action));
    }
    queue[qlen].kind  = kind;
    queue[qlen].a     = a;
    queue[qlen].b     = b;
    queue[qlen].delay = (int64_t)(delay_ms * cyclesPerMs);
    queue[qlen].text  = NULL;
    qlen++;
}

static void push_move(int dx, int dy, int unit) {
    while (dx || dy) {
        int sx = dx > unit ? unit : dx < -unit ? -unit : dx;
        int sy = dy > unit ? unit : dy < -unit ? -unit : dy;
        push(A_MOVE, sx, sy, mouseStepMs);
        dx -= sx;
        dy -= sy;
    }
}

static void push_key(int mods, SDL_Scancode sc) {
    uint8_t key = Keymap_NeXTKeyForScancode(sc);
    push(A_KEYDOWN, mods, key, KEY_STEP_MS);
    push(A_KEYUP, mods, key, KEY_STEP_MS);
}

/* US layout: the scancode and shift for a character */
static SDL_Scancode char_scancode(char c, int* shift) {
    static const char* plain   = "`1234567890-=[]\;',./";
    static const char* shifted = "~!@#$%^&*()_+{}|:\"<>?";
    static const SDL_Scancode codes[] = {
        SDL_SCANCODE_GRAVE, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
        SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9,
        SDL_SCANCODE_0, SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS, SDL_SCANCODE_LEFTBRACKET,
        SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_SEMICOLON,
        SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH
    };
    const char* p;

    *shift = 0;
    if (c >= 'a' && c <= 'z') return (SDL_Scancode)(SDL_SCANCODE_A + (c - 'a'));
    if (c >= 'A' && c <= 'Z') { *shift = 1; return (SDL_Scancode)(SDL_SCANCODE_A + (c - 'A')); }
    if (c == ' ') return SDL_SCANCODE_SPACE;
    if ((p = strchr(plain, c)) != NULL) return codes[p - plain];
    if ((p = strchr(shifted, c)) != NULL) { *shift = 1; return codes[p - shifted]; }
    return SDL_SCANCODE_UNKNOWN;
}

static int modifier(const char* name) {
    if (!strcmp(name, "cmd"))   return NEXTKEY_MOD_LCTRL;   /* NeXT's Command key */
    if (!strcmp(name, "shift")) return NEXTKEY_MOD_LSHIFT;
    if (!strcmp(name, "alt"))   return NEXTKEY_MOD_LALT;
    if (!strcmp(name, "ctrl"))  return NEXTKEY_MOD_META;    /* NeXT's Control key */
    Log_Printf(LOG_WARN, "[Automation] unknown modifier '%s'", name);
    return 0;
}

static void parse(char* line) {
    char* cmd = strtok(line, " \t\r\n");
    char* arg;
    int   right;

    if (!cmd || cmd[0] == '#') return;
    if (!strcmp(cmd, "wait")) {
        arg = strtok(NULL, " \t\r\n");
        push(A_WAIT, 0, 0, arg ? atof(arg) : 0);
    } else if (!strcmp(cmd, "pace")) {
        arg = strtok(NULL, " \t\r\n");
        mouseStepMs = arg ? atof(arg) : MOUSE_STEP_MS;
    } else if (!strcmp(cmd, "move") || !strcmp(cmd, "jump")) {
        char* x = strtok(NULL, " \t\r\n");
        char* y = strtok(NULL, " \t\r\n");
        push_move(x ? atoi(x) : 0, y ? atoi(y) : 0, cmd[0] == 'm' ? 1 : 0x3F);
    } else if (!strcmp(cmd, "down") || !strcmp(cmd, "up") || !strcmp(cmd, "click")) {
        arg = strtok(NULL, " \t\r\n");
        right = arg && !strcmp(arg, "right");
        if (cmd[0] != 'u') push(A_DOWN, right, 0, KEY_STEP_MS * 2);
        if (cmd[0] != 'd') push(A_UP, right, 0, KEY_STEP_MS * 2);
    } else if (!strcmp(cmd, "key")) {
        char* name = strtok(NULL, " \t\r\n");
        int   mods = 0;
        SDL_Scancode sc = name ? SDL_GetScancodeFromName(name) : SDL_SCANCODE_UNKNOWN;
        while ((arg = strtok(NULL, " \t\r\n")) != NULL) mods |= modifier(arg);
        if (sc == SDL_SCANCODE_UNKNOWN) {
            Log_Printf(LOG_WARN, "[Automation] unknown key '%s'", name ? name : "");
            return;
        }
        push_key(mods, sc);
    } else if (!strcmp(cmd, "type")) {
        char* text = strtok(NULL, "\r\n");
        for (; text && *text; text++) {
            int shift;
            SDL_Scancode sc = char_scancode(*text, &shift);
            if (sc != SDL_SCANCODE_UNKNOWN) push_key(shift ? NEXTKEY_MOD_LSHIFT : 0, sc);
        }
    } else if (!strcmp(cmd, "snap")) {
        push(A_SNAP, 0, 0, 0);
    } else if (!strcmp(cmd, "grab")) {
        push(A_GRAB, 0, 0, 0);
    } else if (!strcmp(cmd, "profile")) {
        arg = strtok(NULL, " \t\r\n");
        if (arg && !strcmp(arg, "start")) {
            push(A_PROFILE, AUTOMATION_PROFILE_START, 0, 0);
        } else if (arg && !strcmp(arg, "write") && (arg = strtok(NULL, "\r\n")) != NULL) {
            push(A_PROFILE, AUTOMATION_PROFILE_WRITE, 0, 0);
            queue[qlen - 1].text = strdup(arg);
        } else {
            Log_Printf(LOG_WARN, "[Automation] profile start | profile write FILE");
        }
    } else {
        Log_Printf(LOG_WARN, "[Automation] unknown command '%s'", cmd);
    }
}

/* Grab_Screen into the snapshot directory */
static void grab(void) {
    const char* dir = getenv("PREVIOUS_ND_SNAPSHOT");
    char saved[FILENAME_MAX];
    int  format = ConfigureParams.Printer.nFileFormat;

    if (!dir) {
        Log_Printf(LOG_WARN, "[Automation] grab needs PREVIOUS_ND_SNAPSHOT");
        return;
    }
    snprintf(saved, sizeof(saved), "%s", ConfigureParams.Printer.szPrintToFileName);
    snprintf(ConfigureParams.Printer.szPrintToFileName,
             sizeof(ConfigureParams.Printer.szPrintToFileName), "%s", dir);
#if HAVE_LIBPNG
    ConfigureParams.Printer.nFileFormat = FORMAT_PNG;
#endif
    Grab_Screen();
    ConfigureParams.Printer.nFileFormat = format;
    snprintf(ConfigureParams.Printer.szPrintToFileName,
             sizeof(ConfigureParams.Printer.szPrintToFileName), "%s", saved);
}

static void perform(const Action* a) {
    switch (a->kind) {
        case A_MOVE:    kms_mouse_move(a->a, a->b); break;
        case A_DOWN:    kms_mouse_button(!a->a, true); break;
        case A_UP:      kms_mouse_button(!a->a, false); break;
        case A_KEYDOWN: kms_keydown(a->a, a->b); break;
        case A_KEYUP:   kms_keyup(a->a, a->b); break;
        case A_SNAP:    Automation_SnapshotRequest = 1; break;
        case A_GRAB:    grab(); break;
        case A_PROFILE:
            if (a->text) {
                snprintf(Automation_ProfilePath, sizeof(Automation_ProfilePath), "%s", a->text);
                free(a->text);
            }
            Automation_ProfileRequest = a->a;
            break;
        default:        break;
    }
}

void Automation_Init(void) {
    const char* path = getenv("PREVIOUS_INPUT");

    if (!path || input) return;
    input = fopen(path, "r");
    if (!input) {
        Log_Printf(LOG_WARN, "[Automation] cannot read %s", path);
        return;
    }
    cyclesPerMs = (int64_t)ConfigureParams.System.nCpuFreq * 1000;
    Log_Printf(LOG_WARN, "[Automation] following %s", path);
}

void Automation_Run(int nHostCycles) {
    char line[1024];

    if (!input) return;
    remaining -= nHostCycles;
    while (remaining <= 0) {
        if (qhead == qlen) {
            qhead = qlen = 0;
            while (qlen == 0 && fgets(line, sizeof(line), input)) parse(line);
            if (qlen == 0) {
                clearerr(input);          /* at the end: look again later */
                remaining += POLL_MS * cyclesPerMs;
                return;
            }
        }
        perform(&queue[qhead]);
        remaining += queue[qhead].delay;
        qhead++;
    }
}
