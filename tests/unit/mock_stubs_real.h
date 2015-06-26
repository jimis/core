#include <platform.h>


/**
 *  These are only very simple implementations that call the real functions,
 *  like open(), read(), close() etc. They are only used by mock.c, by the
 *  wrapper functions like __wrap_open(), __wrap_read(), __wrap_close(), so
 *  they need to be present here, even if those wrappers are completely
 *  unused.
 *
 *  To use those __wrap_funcname() wrappers, you need to link using GNU ld's
 *  --wrap=funcname option.
 */


extern int __real_open(const char *filename, int flags, ...);
extern int __real_read(int fd, void *buf, size_t count);
extern int __real_close(int fd);
extern FILE *__real_fopen(const char *filename, const char *mode);
extern int   __real_fclose(FILE *fp);
extern char *__real_fgets(char *buf, int bufsiz, FILE *stream);

