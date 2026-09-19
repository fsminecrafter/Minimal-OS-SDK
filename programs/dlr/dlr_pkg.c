#include "dlr_pkg.h"
#include <string.h>

static void trim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = '\0';

    size_t lead = 0;
    while (s[lead] == ' ' || s[lead] == '\t') lead++;
    if (lead) memmove(s, s + lead, n - lead + 1);
}

static void copy_into(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void lower(char* s) {
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32);
}

void dlr_pkg_init(dlr_pkg* pkg) {
    memset(pkg, 0, sizeof(*pkg));
}

int dlr_pkg_parse(const char* text, size_t len, dlr_pkg* pkg) {
    dlr_pkg_init(pkg);

    char section[64];
    section[0] = '\0';

    char line[512];
    size_t pos = 0;

    while (pos <= len) {
        // Collect one line.
        size_t n = 0;
        while (pos < len && text[pos] != '\n' && n + 1 < sizeof(line)) line[n++] = text[pos++];
        while (pos < len && text[pos] != '\n') pos++;    // overlong line: discard the tail
        line[n] = '\0';
        if (pos >= len && n == 0) break;
        pos++;

        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            char* close = strchr(line, ']');
            if (close) {
                *close = '\0';
                copy_into(section, sizeof(section), line + 1);
                lower(section);
            }
            continue;
        }

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';

        char key[64];
        copy_into(key, sizeof(key), line);
        trim(key);
        lower(key);

        char value[400];
        copy_into(value, sizeof(value), eq + 1);
        trim(value);

        if (strcmp(section, "info") == 0) {
            if      (strcmp(key, "name") == 0)            copy_into(pkg->name, sizeof(pkg->name), value);
            else if (strcmp(key, "version") == 0)         copy_into(pkg->version, sizeof(pkg->version), value);
            else if (strcmp(key, "description") == 0)     copy_into(pkg->description, sizeof(pkg->description), value);
            else if (strcmp(key, "arch") == 0)            copy_into(pkg->arch, sizeof(pkg->arch), value);
            else if (strcmp(key, "operatingsystem") == 0) copy_into(pkg->operatingsystem, sizeof(pkg->operatingsystem), value);
        } else if (strcmp(section, "install.minimalos") == 0) {
            pkg->has_minimalos_section = 1;
            if      (strcmp(key, "copy") == 0)   copy_into(pkg->mos_copy, sizeof(pkg->mos_copy), value);
            else if (strcmp(key, "target") == 0) copy_into(pkg->mos_target, sizeof(pkg->mos_target), value);
            else if (strcmp(key, "run") == 0)    copy_into(pkg->mos_run, sizeof(pkg->mos_run), value);
        }
        // [Install] is deliberately ignored: installscript and
        // installcommand are shell, and there is no shell here. A
        // package that only has those cannot be installed on this
        // platform, and dlr_pkg_runs_here() says so rather than the
        // client half-running something.
    }

    return pkg->name[0] ? 1 : 0;
}

/*
 * Mirrors pkg_compatible() in pkg_parser.cpp closely enough to give
 * the same answer, without the OS bitmask machinery.
 *
 * The important case is the one the C++ side got wrong until
 * recently: a platform token we do not recognise must NOT be treated
 * as "no constraint". Here `operatingsystem=WINDOWS` means the package
 * is for Windows and we say no, rather than shrugging and installing.
 */
int dlr_pkg_runs_here(const dlr_pkg* pkg, const char** why) {
    static const char* reason;
    reason = "";

    if (pkg->arch[0]) {
        char a[16];
        copy_into(a, sizeof(a), pkg->arch);
        lower(a);
        int arch_ok = (strcmp(a, "any") == 0) || (strcmp(a, "x86_64") == 0) ||
                      (strcmp(a, "amd64") == 0) || (strcmp(a, "x64") == 0);
        if (!arch_ok) {
            reason = "package is built for another architecture";
            if (why) *why = reason;
            return 0;
        }
    }

    if (!pkg->operatingsystem[0]) {
        if (why) *why = "";
        return 1;                       // unset means ANY
    }

    char os[96];
    copy_into(os, sizeof(os), pkg->operatingsystem);
    lower(os);

    if (strstr(os, "any")) { if (why) *why = ""; return 1; }
    if (strstr(os, "minimalos")) { if (why) *why = ""; return 1; }

    reason = "package does not list MINIMALOS in operatingsystem";
    if (why) *why = reason;
    return 0;
}

// Splits a comma-separated `copy=` list. Returns 0 when exhausted.
int dlr_pkg_next_copy_entry(const char** cursor, char* out, size_t out_size) {
    const char* p = *cursor;
    if (!p) return 0;

    while (*p == ' ' || *p == ',' || *p == '\t') p++;
    if (!*p) { *cursor = p; return 0; }

    size_t n = 0;
    while (*p && *p != ',' && n + 1 < out_size) out[n++] = *p++;
    while (*p && *p != ',') p++;
    out[n] = '\0';

    while (n && (out[n - 1] == ' ' || out[n - 1] == '\t')) out[--n] = '\0';

    *cursor = (*p == ',') ? p + 1 : p;
    return out[0] ? 1 : 0;
}
