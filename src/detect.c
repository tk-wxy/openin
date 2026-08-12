/*
 * detect.c — 应用自动检索: App Paths 注册表(只读) → PATH → Programs 常用根 → 盘符根。
 * 对外只暴露 detect_app;其余为模块内部。依赖 utils.path_exists。
 */
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

#include "openin.h"

/* 自动检索: 轻量扫描常用根 + 盘符根,首个命中即停 */
static BOOL search_tree(const wchar_t *root, const wchar_t *exeName,
                        int depth, int maxDepth, wchar_t *out)
{
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE hf;

    if (out[0]) return TRUE;
    if (depth > maxDepth) return FALSE;
    _snwprintf_s(pat, MAX_PATH, MAX_PATH - 1, L"%s\\*", root);
    hf = FindFirstFileW(pat, &fd);
    if (hf == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (out[0]) break;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t sub[MAX_PATH];
            _snwprintf_s(sub, MAX_PATH, MAX_PATH - 1, L"%s\\%s", root, fd.cFileName);
            search_tree(sub, exeName, depth + 1, maxDepth, out);
        } else {
            if (_wcsicmp(fd.cFileName, exeName) == 0)
                _snwprintf_s(out, MAX_PATH, MAX_PATH - 1, L"%s\\%s", root, fd.cFileName);
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    return out[0] != L'\0';
}

/* 盘符根一层: 枚举根下子目录,直接探测 exe,不深入 */
static BOOL scan_root_shallow(const wchar_t *root, const wchar_t *exeName, wchar_t *out)
{
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE hf;

    _snwprintf_s(pat, MAX_PATH, MAX_PATH - 1, L"%s*", root);
    hf = FindFirstFileW(pat, &fd);
    if (hf == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t cand[MAX_PATH];
            _snwprintf_s(cand, MAX_PATH, MAX_PATH - 1, L"%s%s\\%s", root, fd.cFileName, exeName);
            if (path_exists(cand)) {
                wcscpy_s(out, MAX_PATH, cand);
                FindClose(hf);
                return TRUE;
            }
        }
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    return FALSE;
}

/* 去掉注册表值两端的空白与引号(部分安装程序登记时带引号/尾随空格) */
static void trim_reg_path(wchar_t *s)
{
    size_t len = wcslen(s);
    while (len > 0 && (s[len - 1] == L' ' || s[len - 1] == L'\t')) s[--len] = L'\0';
    if (len >= 2 && s[0] == L'"' && s[len - 1] == L'"') {
        s[len - 1] = L'\0';
        memmove(s, s + 1, (len - 1) * sizeof(wchar_t));
        len = wcslen(s);
        while (len > 0 && (s[len - 1] == L' ' || s[len - 1] == L'\t')) s[--len] = L'\0';
    }
}

/* 读某个注册表键的默认值(REG_SZ/EXPAND_SZ)到 out,成功返回 TRUE */
static BOOL reg_default_value(HKEY root, const wchar_t *subKey, wchar_t *out, size_t out_sz)
{
    DWORD size = (DWORD)(out_sz * sizeof(wchar_t));
    if (RegGetValueW(root, subKey, NULL, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     NULL, out, &size) != ERROR_SUCCESS)
        return FALSE;
    out[out_sz - 1] = L'\0';
    trim_reg_path(out);
    return out[0] != L'\0';
}

/* App Paths 注册表检测(只读): 安装程序登记的权威位置,零磁盘成本 */
static BOOL detect_app_registry(const wchar_t *exeName, wchar_t *out, size_t out_sz)
{
    static const wchar_t *AP = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths";
    static const wchar_t *AP32 = L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths";
    wchar_t subKey[300];

    out[0] = L'\0';
    _snwprintf_s(subKey, 300, 299, L"%s\\%s", AP, exeName);
    if (reg_default_value(HKEY_CURRENT_USER, subKey, out, out_sz)) return TRUE;
    if (reg_default_value(HKEY_LOCAL_MACHINE, subKey, out, out_sz)) return TRUE;
    _snwprintf_s(subKey, 300, 299, L"%s\\%s", AP32, exeName);
    if (reg_default_value(HKEY_LOCAL_MACHINE, subKey, out, out_sz)) return TRUE;
    out[0] = L'\0';
    return FALSE;
}

/* 自动检索主程序完整路径;成功返回 TRUE */
/* fromFallback=1 时不再做 .exe<->.cmd 扩展名回退,避免两两互检导致无限递归 */
static BOOL detect_app_impl(const wchar_t *exeName, wchar_t *out, size_t out_sz, int fromFallback)
{
    wchar_t root[MAX_PATH], buf[MAX_PATH];
    DWORD len, drives;
    int i;

    out[0] = L'\0';

    /* 1. App Paths 注册表(HKCU/HKLM,安装程序登记的权威位置) */
    if (detect_app_registry(exeName, out, out_sz) && path_exists(out)) return TRUE;

    /* 2. PATH 目录 */
    len = GetEnvironmentVariableW(L"Path", NULL, 0);
    if (len && len < 32768) {
        wchar_t *path = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
        if (path) {
            const wchar_t *p;
            GetEnvironmentVariableW(L"Path", path, len + 1);
            p = path;
            while (*p && !out[0]) {
                const wchar_t *end = wcschr(p, L';');
                size_t dlen = end ? (size_t)(end - p) : wcslen(p);
                if (dlen < MAX_PATH) {
                    memcpy(root, p, dlen * sizeof(wchar_t));
                    root[dlen] = L'\0';
                    _snwprintf_s(buf, MAX_PATH, MAX_PATH - 1, L"%s\\%s", root, exeName);
                    if (path_exists(buf)) wcscpy_s(out, out_sz, buf);
                }
                if (!end) break;
                p = end + 1;
            }
            free(path);
        }
    }
    if (out[0]) return TRUE;

    /* 3. Programs 常用根(深度 2) */
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", root, MAX_PATH)) {
        _snwprintf_s(buf, MAX_PATH, MAX_PATH - 1, L"%s\\Programs", root);
        search_tree(buf, exeName, 0, 2, out);
    }
    if (!out[0] && GetEnvironmentVariableW(L"ProgramFiles", root, MAX_PATH))
        search_tree(root, exeName, 0, 2, out);
    if (!out[0] && GetEnvironmentVariableW(L"ProgramFiles(x86)", root, MAX_PATH))
        search_tree(root, exeName, 0, 2, out);

    /* 4. 各固定盘根(一层) */
    if (!out[0]) {
        drives = GetLogicalDrives();
        for (i = 0; i < 26; i++) {
            if (drives & (1 << i)) {
                wchar_t drv[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
                if (GetDriveTypeW(drv) == DRIVE_FIXED && scan_root_shallow(drv, exeName, buf))
                    wcscpy_s(out, out_sz, buf);
            }
        }
    }
    /* 5. 扩展名回退策略 (.exe <-> .cmd): 只允许回退一层,防止无限递归 */
    if (!out[0] && !fromFallback) {
        wchar_t altName[MAX_PATH];
        wcscpy_s(altName, MAX_PATH, exeName);
        wchar_t *dot = wcsrchr(altName, L'.');
        if (dot) {
            if (_wcsicmp(dot, L".exe") == 0) {
                wcscpy_s(dot, 5, L".cmd");
                detect_app_impl(altName, out, out_sz, 1);
            } else if (_wcsicmp(dot, L".cmd") == 0) {
                wcscpy_s(dot, 5, L".exe");
                detect_app_impl(altName, out, out_sz, 1);
            }
        }
    }
    return out[0] != L'\0';
}

/* 对外接口: 以 exeName 检索,必要时回退到另一扩展名(仅一层) */
BOOL detect_app(const wchar_t *exeName, wchar_t *out, size_t out_sz)
{
    return detect_app_impl(exeName, out, out_sz, 0);
}
