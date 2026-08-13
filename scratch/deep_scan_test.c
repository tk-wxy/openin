/* 深度扫描单元测试: 直接调 deep_scan_presets 打印每行命中。scratch/ 开发工具,不入库。 */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "../src/openin.h"

int wmain(void)
{
    int n = preset_count();
    wchar_t(*found)[MAX_PATH] = (wchar_t(*)[MAX_PATH])calloc((size_t)n, MAX_PATH * sizeof(wchar_t));
    int count, i;
    volatile LONG stop = 0;
    DWORD t0, t1;

    if (!found) return 1;
    t0 = GetTickCount();
    count = deep_scan_presets(found, &stop);
    t1 = GetTickCount();
    wprintf(L"count=%d  time=%lu ms\n", count, (unsigned long)(t1 - t0));
    for (i = 0; i < n; i++)
        wprintf(L"  [%d] %s -> %s\n", i, PRESETS[i].name, found[i][0] ? found[i] : L"(not found)");
    free(found);
    return 0;
}
