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
        close(server_socket);
        unlink(server_path);
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

    /* Receive the file descriptor and the message. */
    char *text = NULL;
    int fd = take_connection(client_socket, &text);
    if (fd < 0)
    {
        return -1;
    }

    /* Write the message to the received descriptor. */
    int result = write(fd, text, strlen(text));
    if (result < 0)
    {
        return -1;
    }

    /* Close the server */
    delete_server();
    free(text);
    return 0;
}
