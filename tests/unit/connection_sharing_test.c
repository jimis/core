/*
   Copyright 2017 Northern.tech AS

   This file is part of CFEngine 3 - written and maintained by Northern.tech AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/


#include <test.h>

#include <string_lib.h>
#include <logging.h>
#include <connection_sharing.h>


static char server_path[] = "/tmp/connection_sharing_test_server";
static const char test_file_tmpl[] = "/tmp/connection_sharing_test_file_XXXXXX";

static void connection_sharing_test(void)
{
    /*
     * This test sends messages between two processes.
     * The child process calls execve to make sure we do not
     * reuse the same file descriptors, otherwise the file descriptor
     * sharing is pointless.
     */
    pid_t child = 0;
    /*
     * Unfortunately one cannot trust vfork to do its job since it has been
     * removed from the standard and it might be an alias for fork. Therefore,
     * the child process will have to do its stuff and the parent will have
     * to retry in case of error.
     */
    unlink(server_path);
    child = vfork();
    assert_int_not_equal(-1, child);
    if (child == 0)
    {
        /* Child */
        char *argv[] = { "connection_sharing_test_helper", NULL };
        char *envp[] = { NULL };
        execve("connection_sharing_test_helper", argv, envp);
        exit(1);
    }
    else
    {
        /*
         * Parent
         * The parent process acts as the sender, the child process is the
         * receiver and runs the local socket server.
         * Check that the socket is created, that indicates that the server is
         * listening and ready to take our command.
         */
        struct stat server;
        int tries = 0;
        do
        {
            if((stat(server_path, &server) == 0) && (S_ISSOCK(server.st_mode)))
            {
                break;
            }
            ++tries;
            sleep(1);
        } while (tries < 10);

        assert_true(tries < 10);

        char test_file[sizeof(test_file_tmpl)];
        strcpy(test_file, test_file_tmpl);
        int fd = mkstemp(test_file);
        assert_int_not_equal(-1, fd);

        /* Send the file descriptor to the child process */
        bool success = share_connection(server_path, fd, test_file);
        assert_true(success);

        /* Wait for the child process to die */
        int status = 0;
        assert_int_not_equal(-1, waitpid(child, &status, 0));
        assert_true(WIFEXITED(status));
        assert_int_equal(0, WEXITSTATUS(status));

        /* Read the contents of the file */
        char buffer[1024];
        memset(buffer, 0, sizeof(buffer));
        assert_int_equal(0, lseek(fd, 0, SEEK_SET));
        assert_int_not_equal(-1, read(fd, buffer, strlen(test_file)));
        assert_string_equal(buffer, test_file);

        /* Test we are not leaking file descriptors: this test runs twice, and
         * the second time the file descriptor should be equal to the first. */
        int null_fd = open("/dev/zero", O_RDONLY);
        static int prev_null_fd = -2;
        if (prev_null_fd == -2)                           /* first test run */
        {
            prev_null_fd = null_fd;
        }
        else                                             /* second test run */
        {
            assert_int_equal(prev_null_fd, null_fd);
        }
        close(null_fd);

        /* Done, close the shop and go home */
        close(fd);
        unlink(test_file);
    }
}

void tests_setup()
{
}

void tests_teardown()
{
}

static char *LogHook(ARG_UNUSED LoggingPrivContext *log_ctx,
                     ARG_UNUSED LogLevel level,
                     const char *message)
{
    return StringConcatenate(2, "sharer> ", message);
}

int main()
{
    LoggingPrivContext log_ctx = {
        .log_hook = LogHook,
        .param = NULL,
    };
    LoggingPrivSetContext(&log_ctx);

    LogSetGlobalLevel(LOG_LEVEL_DEBUG);

    PRINT_TEST_BANNER();
    tests_setup();
    const UnitTest tests[] =
    {
        unit_test(connection_sharing_test),
        /* Run the same test twice, it verifies we are not leaking descriptors. */
        unit_test(connection_sharing_test),
    };
    int result = run_tests(tests);
    tests_teardown();
    return result;
}
