/*
 * pathenv.c — 用户 PATH 操作与安装目录选择。
 * add_to_user_path/path_in_environment/pick_target_dir/find_in_path 导出;
 * path_value_contains/dir_is_writable 为模块内部。
 */
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

#include "openin.h"

/* ---------- 用户 PATH 操作 ---------- */
static BOOL path_value_contains(const wchar_t *value, const wchar_t *dir)
{
    const wchar_t *p = value;
    while (*p) {
        const wchar_t *end = wcschr(p, L';');
        size_t len = end ? (size_t)(end - p) : wcslen(p);
        wchar_t entry[MAX_PATH];
        wchar_t dirmod[MAX_PATH];
        size_t i, elen;

        if (len >= MAX_PATH) len = MAX_PATH - 1;
        for (i = 0; i < len; i++) entry[i] = p[i];
        entry[len] = L'\0';

        elen = wcslen(entry);
        while (elen > 0 && entry[elen - 1] == L'\\') entry[--elen] = L'\0';

        wcscpy_s(dirmod, MAX_PATH, dir);
        elen = wcslen(dirmod);
        while (elen > 0 && dirmod[elen - 1] == L'\\') dirmod[--elen] = L'\0';

        if (entry[0] && _wcsicmp(entry, dirmod) == 0) return TRUE;

        if (!end) break;
        p = end + 1;
    }
    return FALSE;
}

/* 读取用户 PATH 值(type 原样保留;无值视为空,成功返回 TRUE;读取错误返回 FALSE) */
static BOOL read_user_path_value(DWORD *type, wchar_t **val)
{
    DWORD size = 0;
    LONG r;

    *val = NULL;
    r = RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"Path",
                     RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, type, NULL, &size);
    if (r == ERROR_SUCCESS && size > 0) {
        wchar_t *v = (wchar_t *)malloc(size + sizeof(wchar_t));
        if (!v) return FALSE;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"Path",
                         RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                         type, v, &size) != ERROR_SUCCESS) {
            free(v);
            return FALSE;
        }
        v[size / sizeof(wchar_t)] = L'\0';
        *val = v;
        return TRUE;
    }
    return r == ERROR_FILE_NOT_FOUND;
}

BOOL add_to_user_path(const wchar_t *dir)
{
    DWORD type = REG_EXPAND_SZ;
    wchar_t *val = NULL, *newval = NULL;
    size_t oldlen, dirlen;
    BOOL ok = FALSE;
    HKEY hk;

    if (!read_user_path_value(&type, &val)) return FALSE;

    if (val && path_value_contains(val, dir)) {           /* 已在 PATH 中 */
        free(val);
        return TRUE;
    }

    oldlen = val ? wcslen(val) : 0;
    dirlen = wcslen(dir);
    newval = (wchar_t *)malloc((oldlen + dirlen + 2) * sizeof(wchar_t));
    if (!newval) { free(val); return FALSE; }

    if (val && oldlen) {
        memcpy(newval, val, oldlen * sizeof(wchar_t));
        newval[oldlen] = L';';
        oldlen++;
    }
    wcscpy_s(newval + oldlen, dirlen + 1, dir);           /* 尾部追加 dir */
    free(val);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0,
                      KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
        if (RegSetValueExW(hk, L"Path", 0, type,
                           (const BYTE *)newval,
                           (DWORD)((wcslen(newval) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS)
            ok = TRUE;
        RegCloseKey(hk);
    }
    free(newval);

    /* 广播环境变更,让 Explorer 和已打开的资源管理器刷新 */
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
    return ok;
}

/* 从用户 PATH 中移除 dir 条目(与 add_to_user_path 镜像,幂等;只撤自己追加的那份) */
BOOL remove_from_user_path(const wchar_t *dir)
{
    DWORD type = REG_EXPAND_SZ;
    wchar_t *val = NULL, *newval = NULL;
    wchar_t dirmod[MAX_PATH];
    HKEY hk;
    BOOL ok = FALSE, removed = FALSE;
    size_t outlen = 0, dlen;

    if (!read_user_path_value(&type, &val)) return FALSE;
    if (!val || !path_value_contains(val, dir)) {
        free(val);
        return TRUE;                                     /* 无此条目,无事可做 */
    }

    /* 匹配语义与 path_value_contains 一致: 去尾反斜杠 + 忽略大小写,不剥引号 */
    wcscpy_s(dirmod, MAX_PATH, dir);
    dlen = wcslen(dirmod);
    while (dlen > 0 && dirmod[dlen - 1] == L'\\') dirmod[--dlen] = L'\0';

    newval = (wchar_t *)malloc((wcslen(val) + 2) * sizeof(wchar_t));
    if (!newval) { free(val); return FALSE; }
    newval[0] = L'\0';

    {
        const wchar_t *p = val;
        while (*p) {
            const wchar_t *end = wcschr(p, L';');
            size_t seglen = end ? (size_t)(end - p) : wcslen(p);
            size_t len = seglen;                          /* 匹配比较用,截断到缓冲 */
            wchar_t entry[MAX_PATH];
            size_t i, elen;

            if (len >= MAX_PATH) len = MAX_PATH - 1;
            for (i = 0; i < len; i++) entry[i] = p[i];
            entry[len] = L'\0';

            elen = wcslen(entry);
            while (elen > 0 && entry[elen - 1] == L'\\') entry[--elen] = L'\0';

            if (!removed && entry[0] && _wcsicmp(entry, dirmod) == 0) {
                removed = TRUE;                          /* 删除首个匹配条目 */
            } else {
                if (outlen > 0) newval[outlen++] = L';'; /* 其余条目原样保留(含空条目),不截断 */
                memcpy(newval + outlen, p, seglen * sizeof(wchar_t));
                outlen += seglen;
                newval[outlen] = L'\0';
            }
            if (!end) break;
            p = end + 1;
        }
    }

    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0,
                      KEY_SET_VALUE, &hk) == ERROR_SUCCESS) {
        if (RegSetValueExW(hk, L"Path", 0, type,
                           (const BYTE *)newval,
                           (DWORD)((wcslen(newval) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS)
            ok = TRUE;
        RegCloseKey(hk);
    }
    free(newval);
    free(val);

    /* 广播环境变更,让 Explorer 和已打开的资源管理器刷新 */
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000, NULL);
    return ok;
}

/* ---------- 安装目录选择 ---------- */
/* 目录存在且可写? */
static BOOL dir_is_writable(const wchar_t *dir)
{
    DWORD attrs = GetFileAttributesW(dir);
    wchar_t probe[MAX_PATH];
    HANDLE h;

    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        return FALSE;
    _snwprintf_s(probe, MAX_PATH, MAX_PATH - 1, L"%s\\.wtest-%lu", dir,
                 (unsigned long)GetCurrentProcessId());
    h = CreateFileW(probe, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle(h);
    DeleteFileW(probe);
    return TRUE;
}

/* 目录是否已存在于当前环境 PATH(合并了用户+系统) */
BOOL path_in_environment(const wchar_t *dir)
{
    DWORD len = GetEnvironmentVariableW(L"Path", NULL, 0);
    wchar_t *val;
    BOOL f;

    if (!len) return FALSE;
    val = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!val) return FALSE;
    GetEnvironmentVariableW(L"Path", val, len + 1);
    f = path_value_contains(val, dir);
    free(val);
    return f;
}

/*
 * 选择安装目录(写入 out),优先级:
 *   1. -p 覆盖参数
 *   2. %USERPROFILE%\.local\bin   (GitHub 风格用户 bin,claude 也在此,通常已在 PATH)
 *   3. %APPDATA%\npm              (Node 全局工具目录,codex 等在此)
 *   4. 创建 %USERPROFILE%\.local\bin
 *   5. 回退: 创建 %LOCALAPPDATA%\Programs\openin
 */
void pick_target_dir(const wchar_t *override, wchar_t *out, size_t out_sz)
{
    wchar_t tmp[MAX_PATH];
    wchar_t p[MAX_PATH];
    DWORD n;

    if (override && override[0]) {                    /* 1. 显式指定 */
        wcscpy_s(out, out_sz, override);
        return;
    }
    n = GetEnvironmentVariableW(L"USERPROFILE", tmp, MAX_PATH);  /* 2. ~\.local\bin */
    if (n && n < MAX_PATH) {
        _snwprintf_s(out, out_sz, out_sz - 1, L"%s\\.local\\bin", tmp);
        if (dir_is_writable(out)) return;
    }
    n = GetEnvironmentVariableW(L"APPDATA", tmp, MAX_PATH);      /* 3. %APPDATA%\npm */
    if (n && n < MAX_PATH) {
        _snwprintf_s(out, out_sz, out_sz - 1, L"%s\\npm", tmp);
        if (dir_is_writable(out)) return;
    }
    n = GetEnvironmentVariableW(L"USERPROFILE", tmp, MAX_PATH);  /* 4. 创建 ~\.local\bin */
    if (n && n < MAX_PATH) {
        _snwprintf_s(p, MAX_PATH, MAX_PATH - 1, L"%s\\.local", tmp);
        CreateDirectoryW(p, NULL);
        _snwprintf_s(out, out_sz, out_sz - 1, L"%s\\bin", p);
        CreateDirectoryW(out, NULL);
        if (dir_is_writable(out)) return;
    }
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", tmp, MAX_PATH); /* 5. 回退专属目录 */
    if (n && n < MAX_PATH) {
        _snwprintf_s(p, MAX_PATH, MAX_PATH - 1, L"%s\\Programs", tmp);
        CreateDirectoryW(p, NULL);
        _snwprintf_s(out, out_sz, out_sz - 1, L"%s\\openin", p);
        CreateDirectoryW(out, NULL);
        return;
    }
    wcscpy_s(out, out_sz, L"C:\\");
}
BOOL find_in_path(const wchar_t *file, wchar_t *out, size_t out_sz)
{
    DWORD len = GetEnvironmentVariableW(L"Path", NULL, 0);
    wchar_t *path, *p;
    BOOL found = FALSE;

    if (!len) return FALSE;
    path = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (!path) return FALSE;
    GetEnvironmentVariableW(L"Path", path, len + 1);

    p = path;
    while (*p) {
        const wchar_t *end = wcschr(p, L';');
        size_t dlen = end ? (size_t)(end - p) : wcslen(p);
        wchar_t dir[MAX_PATH], cand[MAX_PATH];
        DWORD attrs;

        if (dlen >= MAX_PATH) dlen = MAX_PATH - 1;
        memcpy(dir, p, dlen * sizeof(wchar_t));
        dir[dlen] = L'\0';

        _snwprintf_s(cand, MAX_PATH, MAX_PATH - 1, L"%s\\%s", dir, file);
        attrs = GetFileAttributesW(cand);
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            wcscpy_s(out, out_sz, cand);
            found = TRUE;
            break;
        }
        if (!end) break;
        p = end + 1;
    }
    free(path);
    return found;
}
