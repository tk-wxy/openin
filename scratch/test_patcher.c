#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

/* 通用 patcher:修改 launcher 模板中的目标路径 */
int main(int argc, char *argv[])
{
    FILE *f, *fout;
    long sz, found_offset = -1, i;
    unsigned char *buf;
    wchar_t magic[] = L"__OPENIN_TARGET_EXE_MAGIC";
    size_t magic_bytes_len = wcslen(magic) * sizeof(wchar_t);
    wchar_t new_path[1024];
    wchar_t *wpath;
    int len;

    if (argc < 4) {
        printf("Usage: test_patcher <input.exe> <target_path> <output.exe>\n");
        printf("Example: test_patcher launcher_cli_v2.exe C:\\Windows\\System32\\notepad.exe test_notepad.exe\n");
        return 1;
    }

    /* 打开输入文件 */
    f = fopen(argv[1], "rb");
    if (!f) {
        printf("Failed to open input: %s\n", argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(sz);
    if (!buf) {
        fclose(f);
        printf("Memory allocation failed\n");
        return 1;
    }
    fread(buf, 1, sz, f);
    fclose(f);

    /* 查找 magic string */
    for (i = 0; i <= sz - (long)magic_bytes_len; i++) {
        if (memcmp(buf + i, magic, magic_bytes_len) == 0) {
            found_offset = i;
            break;
        }
    }

    if (found_offset < 0) {
        printf("Magic string not found in %s!\n", argv[1]);
        free(buf);
        return 1;
    }

    printf("Magic found at offset: %ld (0x%lX)\n", found_offset, found_offset);

    /* 将目标路径转为宽字符 */
    len = MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, NULL, 0);
    wpath = (wchar_t *)malloc(len * sizeof(wchar_t));
    if (!wpath) {
        printf("Memory allocation failed\n");
        free(buf);
        return 1;
    }
    MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, wpath, len);

    /* 覆盖 1024 宽字符槽位 */
    ZeroMemory(new_path, sizeof(new_path));
    wcsncpy_s(new_path, 1024, wpath, _TRUNCATE);
    free(wpath);

    memcpy(buf + found_offset, new_path, sizeof(new_path));

    /* 写出 */
    fout = fopen(argv[3], "wb");
    if (!fout) {
        printf("Failed to write output: %s\n", argv[3]);
        free(buf);
        return 1;
    }
    fwrite(buf, 1, sz, fout);
    fclose(fout);
    free(buf);

    printf("Successfully patched: %s -> %s\n", argv[1], argv[3]);
    printf("Target path: %s\n", argv[2]);
    return 0;
}
