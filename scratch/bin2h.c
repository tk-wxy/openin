#include <stdio.h>
#include <stdlib.h>

void dump_file(const char *varname, const char *filepath, FILE *fout) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        printf("Failed to open %s\n", filepath);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(sz);
    fread(buf, 1, sz, f);
    fclose(f);

    fprintf(fout, "/* Automatically generated from %s (%ld bytes) */\n", filepath, sz);
    fprintf(fout, "static const unsigned char %s[] = {\n", varname);
    for (long i = 0; i < sz; i++) {
        fprintf(fout, "0x%02x", buf[i]);
        if (i < sz - 1) fprintf(fout, ", ");
        if ((i + 1) % 16 == 0) fprintf(fout, "\n");
    }
    fprintf(fout, "\n};\n");
    fprintf(fout, "static const size_t %s_len = %ld;\n\n", varname, sz);
    free(buf);
}

int main() {
    FILE *fout = fopen("launcher_templates.h", "w");
    if (!fout) { printf("Failed to open launcher_templates.h for writing\n"); return 1; }

    fprintf(fout, "/* openin launcher precompiled binary templates */\n");
    fprintf(fout, "#ifndef LAUNCHER_TEMPLATES_H\n#define LAUNCHER_TEMPLATES_H\n\n");

    dump_file("g_launcher_gui_exe", "scratch/launcher_gui.exe", fout);
    dump_file("g_launcher_cli_exe", "scratch/launcher_cli.exe", fout);

    fprintf(fout, "#endif /* LAUNCHER_TEMPLATES_H */\n");
    fclose(fout);
    printf("Successfully generated launcher_templates.h!\n");
    return 0;
}
