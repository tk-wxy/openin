/*
 * utils.c — 纯工具: 文件夹选择、路径存在性、主程序定位、命令名校验。
 * 零依赖,只 include openin.h。
 */
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

#include "openin.h"

/* ---------- 文件夹选择 ---------- */
BOOL browse_for_folder(wchar_t *out, size_t out_sz)
{
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;

    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = NULL;
    bi.lpszTitle = L"请选择应用安装目录 (包含主程序 exe 的文件夹)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return FALSE;
    BOOL ok = SHGetPathFromIDListW(pidl, out);
    CoTaskMemFree(pidl);
    return ok && out[0];
}

BOOL path_exists(const wchar_t *p)
{
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

/* 在 folder 中找主程序 exeName;找不到则向上一级再找(兼容选中 bin 子目录的情况) */
BOOL locate_main_exe(const wchar_t *folder, const wchar_t *exeName,
                     wchar_t *out, size_t out_sz)
{
    wchar_t cand[MAX_PATH];
    wchar_t parent[MAX_PATH];
    wchar_t *slash;

    _snwprintf_s(cand, MAX_PATH, MAX_PATH - 1, L"%s\\%s", folder, exeName);
    if (path_exists(cand)) {
        wcscpy_s(out, out_sz, cand);
        return TRUE;
    }
    wcscpy_s(parent, MAX_PATH, folder);
    slash = wcsrchr(parent, L'\\');
    if (slash) *slash = L'\0';
    if (slash && parent[0]) {
        _snwprintf_s(cand, MAX_PATH, MAX_PATH - 1, L"%s\\%s", parent, exeName);
        if (path_exists(cand)) {
            wcscpy_s(out, out_sz, cand);
            return TRUE;
        }
    }
    out[0] = L'\0';
    return FALSE;
}
/* ---------- 命令名校验 ---------- */
int valid_name(const wchar_t *s)
{
    const wchar_t *c;
    if (!s || !*s) return 0;
    for (c = s; *c; c++)
        if (!((*c >= L'a' && *c <= L'z') || (*c >= L'A' && *c <= L'Z') ||
              (*c >= L'0' && *c <= L'9') || *c == L'_' || *c == L'-'))
            return 0;
    return 1;
}
