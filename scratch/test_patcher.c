#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main()
{
    FILE *f = fopen("scratch/launcher_gui.exe", "rb");
    if (!f) { printf("Failed to open input\n"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(sz);
    fread(buf, 1, sz, f);
    fclose(f);

    /* UTF-16LE for "__OPENIN_TARGET_EXE_MAGIC" */
    wchar_t magic[] = L"__OPENIN_TARGET_EXE_MAGIC";
    size_t magic_bytes_len = wcslen(magic) * sizeof(wchar_t);

    long found_offset = -1;
    for (long i = 0; i <= sz - (long)magic_bytes_len; i++) {
        if (memcmp(buf + i, magic, magic_bytes_len) == 0) {
            found_offset = i;
            break;
        }
    }

    if (found_offset < 0) {
        printf("Magic not found!\n");
        free(buf);
        return 1;
    }

    printf("Magic found at offset: %ld (0x%lX)\n", found_offset, found_offset);

    /* Overwrite magic buffer with notepad path */
    wchar_t new_path[1024];
    ZeroMemory(new_path, sizeof(new_path));
    wcscpy_s(new_path, 1024, L"C:\\Windows\\System32\\notepad.exe");

    memcpy(buf + found_offset, new_path, sizeof(new_path));

    FILE *fout = fopen("scratch/patched_notepad.exe", "wb");
    if (!fout) { printf("Failed to write output\n"); free(buf); return 1; }
    fwrite(buf, 1, sz, fout);
    fclose(fout);
    free(buf);
    printf("Successfully wrote scratch/patched_notepad.exe!\n");
    return 0;
}
