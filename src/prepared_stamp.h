/* game/release.txt: the stamp deadnation-setup writes once it has prepared game/ (setup/setup.cpp)
 * and that deadnation checks at every start (src/launcher.c). It names the release (release.txt's
 * first line) and the size and last write time of the dump's and the update's eboot.bin:
 *     release=<release.txt's first line>
 *     dump=<size> <time>
 *     update=<size> <time>
 * When any of them changes (a newer release unpacked over the folder, another dump or update),
 * game/ is prepared again, and the setup first checks the files are the ones this release supports.
 * Header only, for the launcher (C) and the setup (C++). Paths are UTF-8. */
#ifndef DEADNATION_PREPARED_STAMP_H
#define DEADNATION_PREPARED_STAMP_H

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define DEADNATION_STAMP_SEP "\\"
#else
#include <sys/stat.h>
#define DEADNATION_STAMP_SEP "/"
#endif

#define DEADNATION_STAMP_CHARS 1024

/* fopen for a UTF-8 path (Windows: through the wide API, whatever the code page). */
static inline FILE *deadnation_stamp_open(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wide[MAX_PATH * 4], wide_mode[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH * 4) ||
        !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wide_mode, 8)) return NULL;
    return _wfopen(wide, wide_mode);
#else
    return fopen(path, mode);
#endif
}

/* "<size> <last write time>" of a file: Windows' FILETIME, Linux's seconds.nanoseconds (as GNU
 * stat -c '%s %.9Y' prints them). 0 when it is not a file. */
static inline int deadnation_stamp_file(const char *path, char *out, size_t size) {
#ifdef _WIN32
    wchar_t wide[MAX_PATH * 4];
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH * 4) ||
        !GetFileAttributesExW(wide, GetFileExInfoStandard, &data) || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0;
    unsigned long long bytes = ((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    unsigned long long time = ((unsigned long long)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
    snprintf(out, size, "%llu %llu", bytes, time);
#else
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return 0;
    snprintf(out, size, "%llu %llu.%09ld", (unsigned long long)st.st_size, (unsigned long long)st.st_mtim.tv_sec,
             (long)st.st_mtim.tv_nsec);
#endif
    return 1;
}

/* The stamp for the release folder `root` with the dump `game` and the update `update`; 0 when
 * release.txt or either eboot.bin is missing. */
static inline int deadnation_prepared_stamp(const char *root, const char *game, const char *update, char *out, size_t size) {
    char path[4096 + 64], release[256] = "", dump[64], patch[64];
    snprintf(path, sizeof path, "%s" DEADNATION_STAMP_SEP "release.txt", root);
    FILE *f = deadnation_stamp_open(path, "rb");
    if (!f) return 0;
    if (!fgets(release, sizeof release, f)) release[0] = 0;
    fclose(f);
    release[strcspn(release, "\r\n")] = 0;
    if (!release[0] || !update[0]) return 0;
    snprintf(path, sizeof path, "%s" DEADNATION_STAMP_SEP "eboot.bin", game);
    if (!deadnation_stamp_file(path, dump, sizeof dump)) return 0;
    snprintf(path, sizeof path, "%s" DEADNATION_STAMP_SEP "eboot.bin", update);
    if (!deadnation_stamp_file(path, patch, sizeof patch)) return 0;
    snprintf(out, size, "release=%s\ndump=%s\nupdate=%s\n", release, dump, patch);
    return 1;
}

/* 1 when `root`/game/release.txt is the stamp for these folders. */
static inline int deadnation_prepared(const char *root, const char *game, const char *update) {
    char want[DEADNATION_STAMP_CHARS], have[DEADNATION_STAMP_CHARS], path[4096 + 64];
    if (!deadnation_prepared_stamp(root, game, update, want, sizeof want)) return 0;
    snprintf(path, sizeof path, "%s" DEADNATION_STAMP_SEP "game" DEADNATION_STAMP_SEP "release.txt", root);
    FILE *f = deadnation_stamp_open(path, "rb");
    if (!f) return 0;
    size_t n = fread(have, 1, sizeof have - 1, f);
    fclose(f);
    have[n] = 0;
    return !strcmp(have, want);
}

#endif
