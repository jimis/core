#include <test.h>

#include <sys/select.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <string_lib.h>
#include <logging.h>
#include <connection_sharing.h>

static char server_path[] = "/tmp/connection_sharing_test_server";
static int server_socket = -1;

static int create_server(void)
{
    struct sockaddr_un server_address;
    unlink(server_path);
    server_socket = socket(PF_UNIX, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        return -1;
    }
    memset(&server_address, 0, sizeof(struct sockaddr_un));
    server_address.sun_family = AF_UNIX;
    strncpy(server_address.sun_path, server_path, sizeof(server_address.sun_path)-1);

    if (bind(server_socket, (struct sockaddr *)&server_address,
             sizeof(struct sockaddr_un)) < 0)
    {
        return -1;
    }

    if (listen(server_socket, 10) < 0)
    {
        return -1;
    }

    return 0;
}

static int delete_server(void)
{
    if (server_socket > 0)
    {
        close (server_socket);
    }
    return 0;
}

static char *LogHook(ARG_UNUSED LoggingPrivContext *log_ctx,
                     ARG_UNUSED LogLevel level,
                     const char *message)
{
    return StringConcatenate(2, " taker> ", message);
}

int main(void)
{
    LoggingPrivContext log_ctx = {
        .log_hook = LogHook,
        .param = NULL,
    };
    LoggingPrivSetContext(&log_ctx);

    LogSetGlobalLevel(LOG_LEVEL_DEBUG);

    putenv("CFENGINE_TEST_OVERRIDE_EXTENSION_LIBRARY_DIR=../../report-collect-plugin/.libs");
    if (create_server() < 0)
    {
        return -1;
    }
    /* Start accepting connections */
    fd_set rfds;
    struct timeval tv;

    FD_ZERO(&rfds);
    FD_SET(server_socket, &rfds);
    tv.tv_sec = 10;
    tv.tv_usec = 0;

    int number = select(server_socket+1, &rfds, NULL, NULL, &tv);
    if ((number < 0) || !FD_ISSET(server_socket, &rfds))
    {
        return -1;
    }

    /* Accept the connection */
    struct sockaddr_un client_address;
    socklen_t client_address_size = sizeof(struct sockaddr_un);
    int client_socket = accept(server_socket, (struct sockaddr *)&client_address,
                               &client_address_size);
    if (client_socket < 0)
    {
        return -1;
    }

    char *filename = NULL;
    int fd = take_connection(client_socket, &filename);

    if (fd < 0)
    {
        return -1;
    }
    int result = write(fd, filename, strlen(filename));
    if (result < 0)
    {
        return -1;
    }
    fsync(fd);

    /* Close the server */
    delete_server();
    return 0;
}
