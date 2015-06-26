#include <test.h>
#include <mock.h>
#include <mock_stubs_real.h>

#include <alloc.h>

/**
   @TODO configure test for ld --wrap=blah
 */


#define MOCK_MAX_OBJECTS 10

Mock ACTIVE_MOCKS[MOCK_MAX_OBJECTS];


#define MOCK_MAGIC_BASE 1234567

static int _Mock_NextMagicNumber(void)
{
    static int monotonic_increment = 0;

    int magic_number = MOCK_MAGIC_BASE + monotonic_increment;
    monotonic_increment++;

    return magic_number;
}

Mock *_Mock_FindFileDescriptor(int fd)
{
    for (int i = 0; i < MOCK_MAX_OBJECTS; i++)
    {
        if (ACTIVE_MOCKS[i].file_descriptor == fd)
        {
            return &ACTIVE_MOCKS[i];
        }
    }

    return NULL;
}

Mock *_Mock_FindPointer(void *p)
{
    for (int i = 0; i < MOCK_MAX_OBJECTS; i++)
    {
        if (ACTIVE_MOCKS[i].pointer == p)
        {
            return &ACTIVE_MOCKS[i];
        }
    }

    return NULL;
}

Mock *_Mock_FindFilename(const char *filename)
{
    for (int i = 0; i < MOCK_MAX_OBJECTS; i++)
    {
        if (ACTIVE_MOCKS[i].filename != NULL &&
            strcmp(ACTIVE_MOCKS[i].filename, filename) == 0)
        {
            return &ACTIVE_MOCKS[i];
        }
    }

    return NULL;
}


/**
 *  SYSTEM CALL WRAPPERS
 *
 *  @NOTE Enable tests using this kind of mocking only with GNU linker.
 *        Link with --wrap=open for example, to enable the open() wrapper.
 */



int __wrap_open(const char *filename, int flags, ...)
{
    Mock *mock = _Mock_FindFilename(filename);

    if (mock == NULL)                                         /* no mocking */
    {
        va_list ap;
        va_start(ap, flags);
        int retval = __real_open(filename, flags, ap);
        va_end(ap);
        return retval;
    }

    assert(strcmp(filename, mock->filename) == 0);

    mock->file_descriptor = _Mock_NextMagicNumber();
    mock->file_position   = 0;

    return mock->file_descriptor;
}
int __wrap_read(int fd, void *buf, size_t count)
{
    Mock *mock = _Mock_FindFileDescriptor(fd);
    if (mock == NULL)                                         /* no mocking */
    {
        return __real_read(fd, buf, count);
    }

    assert(fd == mock->file_descriptor);
    assert(mock->file_position <= mock->file_content_len + 1);

    if    (mock->file_position == mock->file_content_len + 1)
    {
        return 0;                                               /* EOF */
    }

    char      *s = &mock->file_content[mock->file_position];
    size_t s_len =  mock->file_content_len - mock->file_position;
    size_t read_size = MIN(s_len + 1, count);

    memcpy(buf, s, read_size);
    mock->file_position += read_size;

    return read_size;
}
int __wrap_close(int fd)
{
    Mock *mock = _Mock_FindFileDescriptor(fd);

    if (mock == NULL)
    {
        return __real_close(fd);
    }

    assert(fd == mock->file_descriptor);

    mock->file_position = SIZE_MAX;
    return 0;
}
FILE *__wrap_fopen(const char *filename, const char *mode)
{
    Mock *mock = _Mock_FindFilename(filename);

    if (mock == NULL)                                         /* no mocking */
    {
        return __real_fopen(filename, mode);
    }

    assert(strcmp(filename, mock->filename) == 0);

    mock->pointer = (void *) (intptr_t) _Mock_NextMagicNumber();
    mock->file_position = 0;

    return mock->pointer;
}
int __wrap_fclose(FILE *fp)
{
    Mock *mock = _Mock_FindPointer(fp);

    if (mock == NULL)                                         /* no mocking */
    {
        return __real_fclose(fp);
    }

    assert(fp == mock->pointer);

    mock->file_position = SIZE_MAX;
    return 0;
}
char *__wrap_fgets(char *buf, int bufsiz, FILE *fp)
{
    Mock *mock = _Mock_FindPointer(fp);

    if (mock == NULL)                                         /* no mocking */
    {
        return __real_fgets(buf, bufsiz, fp);
    }

    assert(fp == mock->pointer);
    assert(mock->file_position <= mock->file_content_len + 1);

    if    (mock->file_position == mock->file_content_len + 1)
    {
        return NULL;                                            /* EOF */
    }

    /* Current position in the mocked file. */
    char      *s = &mock->file_content[mock->file_position];
    size_t s_len =  mock->file_content_len - mock->file_position;

    char *newline = strchr(s, '\n');

    if (newline == NULL)
    {
        /* Copy all the rest of the contents, or as much as it fits. */
        size_t read_size = MIN(s_len, bufsiz - 1);
        memcpy(buf, s, read_size);
        buf[read_size] = '\0';
        mock->file_position += read_size;
    }
    else
    {
        /* Copy all the line including '\n', or as much as it fits. */
        size_t line_len  = newline + 1 - s;
        size_t read_size = MIN(line_len, bufsiz - 1);
        memcpy(buf, s, read_size);
        buf[read_size] = '\0';
        mock->file_position += read_size;
    }

    return buf;

}


/******** EXPORTED FUNCTIONS *********/

Mock *Mock_Filename(const char *filename, const char *content)
{
    for (int i = 0; i < MOCK_MAX_OBJECTS; i++)
    {
        if ( ! ACTIVE_MOCKS[i].active)
        {
            Mock *mock = &ACTIVE_MOCKS[i];

            *mock = (Mock) {
                .active           = true,
                .filename         = xstrdup(filename),
                .file_content     = xstrdup(content),
                .file_content_len = strlen(content),
                /* we must open() first */
                .file_position    = SIZE_MAX,
                .file_descriptor  = -1
            };

            return mock;
        }
    }

    /* No empty mocking slot found! Maybe you're mocking a bit too much... */
    return NULL;
}

void Mock_End(Mock *mock)
{
    free(mock->filename);
    free(mock->file_content);

    *mock = (Mock) { 0 };
}
