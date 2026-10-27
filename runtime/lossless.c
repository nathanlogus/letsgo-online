// Lossless Scaling (Steam app 993090, a program of its own): with the setting on, started from the user's Steam library
// (minimized) and its scale hotkey pressed once the game window has the focus, so it scales this window with the user's
// own Lossless Scaling settings. Turning the setting off presses the hotkey again (scaling off); Lossless Scaling keeps running.
#include "lossless.h"
#include "settings.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

static bool ls_running(void) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe = {.dwSize = sizeof pe};
    bool found = false;
    for (BOOL ok = Process32First(s, &pe); ok && !found; ok = Process32Next(s, &pe)) found = !_stricmp(pe.szExeFile, "LosslessScaling.exe");
    CloseHandle(s);
    return found;
}

// LosslessScaling.exe in one of Steam's libraries (Steam's path from the registry, the others from its libraryfolders.vdf)
static bool ls_path(char out[MAX_PATH]) {
    char steam[MAX_PATH], vdf[MAX_PATH], line[1024];
    DWORD n = sizeof steam;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, NULL, steam, &n) != ERROR_SUCCESS) return false;
    snprintf(out, MAX_PATH, "%s/steamapps/common/Lossless Scaling/LosslessScaling.exe", steam);
    if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return true;
    snprintf(vdf, sizeof vdf, "%s/steamapps/libraryfolders.vdf", steam);
    FILE *f = fopen(vdf, "r");
    if (!f) return false;
    bool ok = false;
    while (!ok && fgets(line, sizeof line, f)) {
        char lib[MAX_PATH], *d = lib;
        const char *p = strstr(line, "\"path\"");
        if (!p || !(p = strchr(p + 6, '"'))) continue;
        for (p++; *p && *p != '"' && d < lib + sizeof lib - 1; p++) {
            if (*p == '\\' && p[1] == '\\') p++;  // (the vdf escapes its backslashes)
            *d++ = *p;
        }
        *d = 0;
        snprintf(out, MAX_PATH, "%s/steamapps/common/Lossless Scaling/LosslessScaling.exe", lib);
        ok = GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
    }
    fclose(f);
    return ok;
}

// The scale hotkey from Lossless Scaling's Settings.xml (<Hotkey>S</Hotkey>, <HotkeyModifierKeys>Alt Control</...>), else
// its default Ctrl+Alt+S. shortcut: letters, digits and F1-F24 only; other keys fall back to the default.
static void ls_hotkey(WORD *key, bool *ctrl, bool *alt, bool *shift, bool *win) {
    *key = 'S', *ctrl = *alt = true, *shift = *win = false;
    char path[MAX_PATH], buf[65536];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", path, MAX_PATH)) return;
    strncat(path, "\\Lossless Scaling\\Settings.xml", MAX_PATH - strlen(path) - 1);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = 0;
    const char *k = strstr(buf, "<Hotkey>"), *m = strstr(buf, "<HotkeyModifierKeys>");
    if (!k || !m) return;
    char name[16] = {0}, mods[64] = {0};
    sscanf(k + 8, "%15[^<]", name);
    sscanf(m + 20, "%63[^<]", mods);
    if (strlen(name) == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9'))) *key = (WORD)name[0];
    else if (name[0] == 'D' && name[1] >= '0' && name[1] <= '9' && !name[2]) *key = (WORD)name[1];  // (WPF's digit keys: D0..D9)
    else if (name[0] == 'F' && atoi(name + 1) >= 1 && atoi(name + 1) <= 24) *key = (WORD)(VK_F1 + atoi(name + 1) - 1);
    else return;
    *ctrl = strstr(mods, "Control") != NULL, *alt = strstr(mods, "Alt") != NULL;
    *shift = strstr(mods, "Shift") != NULL, *win = strstr(mods, "Windows") != NULL;
}

static void press_hotkey(void) {
    WORD key;
    bool mod[4];
    ls_hotkey(&key, &mod[0], &mod[1], &mod[2], &mod[3]);
    static const WORD vk[4] = {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN};
    INPUT in[10] = {0};
    int n = 0;
    for (int i = 0; i < 4; i++) if (mod[i]) in[n].type = INPUT_KEYBOARD, in[n++].ki.wVk = vk[i];
    in[n].type = INPUT_KEYBOARD, in[n++].ki.wVk = key;
    in[n].type = INPUT_KEYBOARD, in[n].ki.wVk = key, in[n++].ki.dwFlags = KEYEVENTF_KEYUP;
    for (int i = 3; i >= 0; i--) if (mod[i]) in[n].type = INPUT_KEYBOARD, in[n].ki.wVk = vk[i], in[n++].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput((UINT)n, in, sizeof *in);
    SDL_Log("[lossless] scale hotkey pressed");
}

void lossless_tick(SDL_Window *win) {
    static bool active, failed;
    static Uint64 ready_at;  // (Lossless Scaling's start-up: its hotkey is registered a few seconds after launch)
    if (!g_settings.lossless) {
        if (active && (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS)) press_hotkey(), active = false;
        failed = false, ready_at = 0;
        return;
    }
    if (active || failed) return;
    if (!ready_at) {
        if (ls_running()) ready_at = SDL_GetTicks() + 1000;
        else {
            char path[MAX_PATH];
            if (!ls_path(path) || (INT_PTR)ShellExecuteA(NULL, "open", path, "-StartMinimized", NULL, SW_SHOWMINNOACTIVE) <= 32) {
                SDL_Log("[lossless] Lossless Scaling not found in the Steam libraries");
                failed = true;
                return;
            }
            SDL_Log("[lossless] started %s", path);
            ready_at = SDL_GetTicks() + 6000;
        }
    }
    if (SDL_GetTicks() < ready_at || !(SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) || !ls_running()) return;
    press_hotkey();  // (it scales the window in the foreground: this one)
    active = true;
}
#else
void lossless_tick(SDL_Window *win) { (void)win; }
#endif
