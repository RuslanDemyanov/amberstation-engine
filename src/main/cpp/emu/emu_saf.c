// Мост SAF -> ядро: файлы из SAF недоступны fopen'ом по пути (scoped storage
// запрещает open(/proc/self/fd/N)), но читать по самому fd можно.
// Линкер-флаг --wrap=fopen перехватывает fopen ядра: путь вида
// "saf:<n>.<ext>" или ".__saf<n>.<ext>" (ссылка из cue) оборачиваем
// в FILE* через fdopen(dup(fd)) — без пере-открытия.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define SAF_MAX_FDS 16
static int g_saf_fds[SAF_MAX_FDS];
static int g_saf_count;

// вызывается из JNI при boot (однопоточно, до старта эмуляции)
int emu_saf_register_fd(int fd)
{
    if (g_saf_count >= SAF_MAX_FDS)
        return -1;
    int dup_fd = dup(fd);
    if (dup_fd < 0)
        return -1;
    g_saf_fds[g_saf_count] = dup_fd;
    return g_saf_count++;
}

void emu_saf_reset(void)
{
    for (int i = 0; i < g_saf_count; i++)
        close(g_saf_fds[i]);
    g_saf_count = 0;
}

extern FILE *__real_fopen(const char *path, const char *mode);

static FILE *saf_open(const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    int idx = -1;
    if (strncmp(base, ".__saf", 6) == 0)
        idx = atoi(base + 6);
    else if (strncmp(path, "saf:", 4) == 0)
        idx = atoi(path + 4);
    if (idx < 0 || idx >= g_saf_count)
        return NULL;

    // dup: у FILE* будет свой дескриптор; offset у разделяемого файла
    // в позиции 0 (pfd никто не читал до этого)
    int fd = dup(g_saf_fds[idx]);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, "rb");
    if (f == NULL)
        close(fd);
    return f;
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (path != NULL && (strncmp(path, "saf:", 4) == 0 ||
                         strstr(path, ".__saf") != NULL)) {
        return saf_open(path);
    }
    return __real_fopen(path, mode);
}
