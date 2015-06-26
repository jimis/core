#include <platform.h>

int __real_open(const char *filename, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int retval = open(filename, flags, ap);
    va_end(ap);
    return retval;
}
int __real_read(int fd, void *buf, size_t count)
{
    return read(fd, buf, count);
}
int __real_close(int fd)
{
    return close(fd);
}
FILE *__real_fopen(const char *filename, const char *mode)
{
    return fopen(filename, mode);
}
int   __real_fclose(FILE *fp)
{
    return fclose(fp);
}
char *__real_fgets(char *buf, int bufsiz, FILE *stream)
{
    return fgets(buf, bufsiz, stream);
}

