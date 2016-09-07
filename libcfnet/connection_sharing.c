/* #include <cf.nova.h> */
/* #include <cf.enterprise.h> */

#include <platform.h>
#include <cfnet.h>
#include <alloc.h>
#include <logging.h>
#include <misc_lib.h>
#include <passopenfile.h>

#include <sys/types.h>
#ifndef __MINGW32__
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#endif // __MINGW32__
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>


/* IPC functions, over a local Unix Domain Socket (UDS), to transmit
 * (remote) socket descriptors along with a payload of related text
 * (host identification).  Note that the communication is between two
 * processes on the same host, so binary communications are valid
 * (albeit best avoided, all the same, in the interests of clarity).
 *
 * It is expected that the receiving end has an open socket bound to
 * the UDS's file as interface and listen()s for connections; the
 * sending end connects such a socket, the receiver accept()s it, they
 * communicate over the resulting channel then shut it down, having
 * transmitted an open socket in the course of their conversation.
 * Thus each UDS used for transmission just transmits one socket (or
 * none, on failure); after that, it's torn down.
 */

/* TODO: replace explicit waiting with use of blocking UDS. */
/* Package a call to select(), to avoid duplicating boilerplate code. */
static bool wait_for(const int uds, bool write, bool *ready)
{
    struct timeval tv;
    fd_set fds;

    FD_ZERO(&fds);
    FD_SET(uds, &fds);
    tv.tv_sec = 1; /* Wait for up to a second */
    tv.tv_usec = 0;

    int ret;
    if (write)
    {
        ret = select(uds + 1, NULL, &fds, NULL, &tv);
    }
    else
    {
        ret = select(uds + 1, &fds, NULL, NULL, &tv);
    }

    if (ret < 0)
    {
        return false;
    }
    *ready = FD_ISSET(uds, &fds);
    return true;
}


bool share_connection(const char * path, const int descriptor,
                      const char *message)
{
    if (!path || (descriptor < 0) || !message)
    {
        ProgrammingError("Invalid arguments");
    }
    int uds = socket(AF_UNIX, SOCK_STREAM, 0);
    if (uds < 0)
    {
        Log(LOG_LEVEL_ERR, "Failed to create a local socket (socket: %s)",
            GetErrorStr());
        return false;
    }
    else if (uds >= FD_SETSIZE)
    {
        Log(LOG_LEVEL_ERR, "Can't send shared connection, "
            "socket descriptor too high (%d >= %d)!",
            uds, FD_SETSIZE);
        cf_closesocket(uds);
        return false;
    }
    else
    {
        /* Prepare the socket */
        struct sockaddr_un remote_address;
        assert(strlen(path) < sizeof(remote_address.sun_path));
        remote_address.sun_family = AF_UNIX;
        strlcpy(remote_address.sun_path, path, sizeof(remote_address.sun_path));

        if (connect(uds, (struct sockaddr *)&remote_address, sizeof(remote_address)) < 0)
        {
            Log(LOG_LEVEL_ERR,
                "Can't connect to local socket at '%s' to share connection (connect: %s)",
                path, GetErrorStr());
            cf_closesocket(uds);
            return false;
        }
    }

    Log(LOG_LEVEL_VERBOSE,
        "Sharing connection (socket:'%s', descriptor:%d, message:'%s')",
        path, descriptor, message);

    bool retval = false;
    bool ready  = false;
    /* Get the socket ready to send the descriptor. */
    if (!wait_for(uds, true, &ready))
    {
        Log(LOG_LEVEL_ERR,
            "Can't share socket to server (select: %s)",
            GetErrorStr());
    }
    else if (!ready)
    {
        Log(LOG_LEVEL_ERR,
            "Can't share socket to server (server not ready)");
    }
    /* Send the descriptor and IP address. */
    else if (PassOpenFile_Put(uds, descriptor, message))
    {
        retval = true;
    }

    cf_closesocket(uds);
    return retval;
}

/* We don't own this uds, so should not close it. */
int take_connection(const int uds, char **message)
{
    if (uds < 0 || !message)
    {
        ProgrammingError("Invalid arguments");
    }
    else if (uds >= FD_SETSIZE)
    {
        Log(LOG_LEVEL_ERR, "Can't receive shared connection, "
            "socket descriptor too high (%d >= %d)!",
            uds, FD_SETSIZE);
        return -1;
    }

    /* Get the socket ready to receive a descriptor: */
    bool ready = false;
    if (!wait_for(uds, false, &ready))
    {
        Log(LOG_LEVEL_ERR,
            "Can't receive shared connection (select: %s)",
            GetErrorStr());
    }
    else if (!ready)
    {
        Log(LOG_LEVEL_VERBOSE, "No shared connection received, continuing");
    }
    else /* Receive a descriptor: */
    {
        return PassOpenFile_Get(uds, message);
    }
    return -1;
}
