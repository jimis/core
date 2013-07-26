/*
   Copyright (C) CFEngine AS

   This file is part of CFEngine 3 - written and maintained by CFEngine AS.

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
  versions of CFEngine, the applicable Commerical Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include "server.h"

#include "item_lib.h"
#include "crypto.h"
#include "files_names.h"
#include "files_interfaces.h"
#include "files_hashes.h"
#include "env_context.h"
#include "lastseen.h"
#include "dir.h"
#include "conversion.h"
#include "matching.h"
#include "string_lib.h"
#include "pipes.h"
#include "signals.h"
#include "mutex.h"
#include "classic.h"                  /* SendSocketStream */
#include "net.h"                      /* SendTransaction,ReceiveTransaction */
#include "tls_generic.h"              /* TLSSend */
#include "rlist.h"
#include "misc_lib.h"
#include "cf-serverd-enterprise-stubs.h"
#include "audit.h"

#include <openssl/ssl.h>

#ifdef HAVE_NOVA
# include "cf.nova.h"
#endif

//******************************************************************
// GLOBAL STATE
//******************************************************************

int CLOCK_DRIFT = 3600;  /* 1hr */
int ACTIVE_THREADS;

int CFD_MAXPROCESSES = 0;
bool DENYBADCLOCKS = true;

int MAXTRIES = 5;
bool LOGENCRYPT = false;
int COLLECT_INTERVAL = 0;
int COLLECT_WINDOW = 10;
bool SERVER_LISTEN = true;

ServerAccess SV;

char CFRUNCOMMAND[CF_BUFSIZE] = { 0 };

//******************************************************************/
// LOCAL CONSTANTS
//******************************************************************/

static const size_t CF_BUFEXT = 128;
static const int CF_NOSIZE = -1;

static void SpawnConnection(EvalContext *ctx, int sd_reply, char *ipaddr);
static void *HandleConnection(ServerConnectionState *conn);
static int BusyWithConnection(EvalContext *ctx, ServerConnectionState *conn);
static int MatchClasses(EvalContext *ctx, ServerConnectionState *conn);
static void DoExec(EvalContext *ctx, ServerConnectionState *conn, char *args);
ProtocolCommand GetCommand(char *str);
static int VerifyConnection(ServerConnectionState *conn, char buf[CF_BUFSIZE]);
static void RefuseAccess(ServerConnectionState *conn, int size, char *errmesg);
static int AccessControl(EvalContext *ctx, const char *req_path, ServerConnectionState *conn, int encrypt);
static int LiteralAccessControl(EvalContext *ctx, char *in, ServerConnectionState *conn, int encrypt);
static Item *ContextAccessControl(EvalContext *ctx, char *in, ServerConnectionState *conn, int encrypt);
static void ReplyServerContext(ServerConnectionState *conn, int encrypted, Item *classes);
static int CheckStoreKey(ServerConnectionState *conn, RSA *key);
static int StatFile(ServerConnectionState *conn, char *sendbuffer, char *ofilename);
static void CfGetFile(ServerFileGetState *args);
static void CfEncryptGetFile(ServerFileGetState *args);
static void CompareLocalHash(ServerConnectionState *conn, char *sendbuffer, char *recvbuffer);
static void GetServerLiteral(EvalContext *ctx, ServerConnectionState *conn, char *sendbuffer, char *recvbuffer, int encrypted);
static int GetServerQuery(ServerConnectionState *conn, char *recvbuffer);
static int CfOpenDirectory(ServerConnectionState *conn, char *sendbuffer, char *oldDirname);
static int CfSecOpenDirectory(ServerConnectionState *conn, char *sendbuffer, char *dirname);
static void Terminate(ConnectionInfo *connection);
static int AllowedUser(char *user);
static int AuthorizeRoles(EvalContext *ctx, ServerConnectionState *conn, char *args);
static int TransferRights(char *filename, ServerFileGetState *args, struct stat *sb);
static void AbortTransfer(ConnectionInfo *connection, char *filename);
static void FailedTransfer(ConnectionInfo *connection);
static void ReplyNothing(ServerConnectionState *conn);
static ServerConnectionState *NewConn(EvalContext *ctx, int sd);
static void DeleteConn(ServerConnectionState *conn);
static int cfscanf(char *in, int len1, int len2, char *out1, char *out2, char *out3);
static int AuthenticationDialogue(ServerConnectionState *conn, char *recvbuffer, int recvlen);
static int OptionFound(char *args, char *pos, char *word);
static int DoStartTLS(ServerConnectionState *connection);

//******************************************************************/
// LOCAL STATE
//******************************************************************/

static const char *PROTOCOL[] =
{
    "EXEC",
    "AUTH",                     /* old protocol */
    "GET",
    "OPENDIR",
    "SYNCH",
    "CLASSES",
    "MD5",
    "SMD5",
    "CAUTH",
    "SAUTH",
    "SSYNCH",
    "SGET",
    "VERSION",
    "SOPENDIR",
    "VAR",
    "SVAR",
    "CONTEXT",
    "SCONTEXT",
    "SQUERY",
    "SCALLBACK",
    "STARTTLS",
    NULL
};
static int TRIES = 0;

/*******************************************************************/


static SSL_CTX *SSLSERVERCONTEXT = NULL;
static X509 *SSLSERVERCERT = NULL;


/**
 * @warning Make sure you've called CryptoInitialize() first!
 */
bool ServerTLSInitialize()
{
    int ret;

    /* OpenSSL is needed for our new protocol over TLS. */
    SSL_library_init();
    SSL_load_error_strings();

    assert(SSLSERVERCONTEXT == NULL);
    SSLSERVERCONTEXT = SSL_CTX_new(SSLv23_server_method());
    if (SSLSERVERCONTEXT == NULL)
    {
        Log(LOG_LEVEL_ERR, "SSL_CTX_new: %s",
            ERR_reason_error_string(ERR_get_error()));
        return false;
    }

    /* Use only TLS v1 or later.
       TODO option for SSL_OP_NO_TLSv{1,1_1} */
    SSL_CTX_set_options(SSLSERVERCONTEXT,
                        SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

    /*
     * Create cert into memory and load it into SSL context.
     */

    if (PRIVKEY == NULL)
    {
        Log(LOG_LEVEL_ERR,
            "No public/private key pair is loaded, create one with cf-key");
        return false;
    }
    assert(SSLSERVERCERT == NULL);
    /* Generate self-signed cert valid from now to 100 years later. */
    X509 *x509 = X509_new();
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_time_adj_ex(X509_get_notAfter(x509), 365*100, 0, NULL);
    EVP_PKEY *pkey = EVP_PKEY_new();
    EVP_PKEY_assign_RSA(pkey, PRIVKEY);
    X509_NAME *name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const char *) "ouripaddress",   /* TODO */
                               -1, -1, 0);
    X509_set_issuer_name(x509, name);
    X509_set_pubkey(x509, pkey);
    X509_sign(x509, pkey, EVP_sha384());

    /* Log(LOG_LEVEL_ERR, "generate cert from priv key: %s", */
    /*     ERR_reason_error_string(ERR_get_error())); */

    SSL_CTX_use_certificate(SSLSERVERCONTEXT, x509);
    SSLSERVERCERT = x509;

    ret = SSL_CTX_use_RSAPrivateKey(SSLSERVERCONTEXT, PRIVKEY);
    if (ret != 1)
    {
        Log(LOG_LEVEL_ERR, "Failed to use RSA private key: %s",
            ERR_reason_error_string(ERR_get_error()));
        return false;
    }
    ret = SSL_CTX_check_private_key(SSLSERVERCONTEXT);    /* verify cert consistency */
    if (ret != 1)
    {
        Log(LOG_LEVEL_ERR, "Inconsistent key and TLS cert: %s",
            ERR_reason_error_string(ERR_get_error()));
        return false;
    }

    /* Set options to always request a certificate from the peer, either we
     * are client or server. */
    SSL_CTX_set_verify(SSLSERVERCONTEXT, SSL_VERIFY_PEER, NULL);
    /* Always accept that certificate, we do proper checking after TLS
     * connection is established since OpenSSL can't pass a connection
     * specific pointer to the callback (so we would have to lock).  */
    SSL_CTX_set_cert_verify_callback(SSLSERVERCONTEXT, TLSVerifyCallback, NULL);

    return true;
}

int ServerStartTLS(ConnectionInfo *conn_info)
{
    int result = 0;

    assert(SSLSERVERCONTEXT != NULL);

    /* Positive reply to client's STARTTLS. */
    result = SendTransaction(conn_info, "ACK", 0, CF_DONE);

    /* Now we wait for the client to initiate TLS handshake, so we're letting
     * OpenSSL take over. */
    conn_info->type = CF_PROTOCOL_TLS;
    conn_info->ssl = SSL_new(SSLSERVERCONTEXT);
    if (conn_info->ssl == NULL)
    {
        Log(LOG_LEVEL_ERR, "SSL_new: %s",
            ERR_reason_error_string(ERR_get_error()));
        return -1;
    }

    /* Initiate the TLS handshake over the already open TCP socket. */
    SSL_set_fd(conn_info->ssl, conn_info->sd);

    int total_tries = 0;
    do {
        result = SSL_accept(conn_info->ssl);
        if (result <= 0)
        {
            /*
             * Identify the problem and if possible try to fix it.
             */
            int error = SSL_get_error(conn_info->ssl, result);
            if ((SSL_ERROR_WANT_WRITE == error) || (SSL_ERROR_WANT_READ == error))
            {
                Log(LOG_LEVEL_DEBUG, "Recoverable error in TLS handshake, trying to fix it");
                /*
                 * We can try to fix this.
                 * This error means that there was not enough data in the buffer, using select
                 * to wait until we get more data.
                 */
                fd_set rfds;
                struct timeval tv;
                int tries = 0;

                do {
                    SET_DEFAULT_TLS_TIMEOUT(tv);
                    FD_ZERO(&rfds);
                    FD_SET(conn_info->sd, &rfds);

                    result = select(conn_info->sd + 1, &rfds, NULL, NULL, &tv);
                    if (result > 0)
                    {
                        /*
                         * Ready to receive data
                         */
                        break;
                    }
                    else
                    {
                        Log(LOG_LEVEL_VERBOSE, "select(2) timed out, retrying (tries: %d)", tries);
                        ++tries;
                    }
                } while (tries <= DEFAULT_TLS_TRIES);
            }
            else
            {
                /*
                 * Unrecoverable error
                 */
                int e = ERR_get_error();
                Log(LOG_LEVEL_ERR, "TLS handshake err %d %d: %s",
                    error, e, ERR_reason_error_string(e));
                return -1;
            }
        }
        else
        {
            /*
             * TLS channel established, start talking!
             */
            Log (LOG_LEVEL_INFO, "TLS session established, checking trust...");
            break;
        }
        ++total_tries;
    } while (total_tries <= DEFAULT_TLS_TRIES);
    return 0;
}

void ServerEntryPoint(EvalContext *ctx, int sd_accepted, char *ipaddr)
{
    char intime[64];
    time_t now;

    Log(LOG_LEVEL_VERBOSE,
        "Obtained IP address of '%s' on socket %d from accept",
        ipaddr, sd_accepted);

    if ((SV.nonattackerlist) && (!IsMatchItemIn(ctx, SV.nonattackerlist, MapAddress(ipaddr))))
    {
        Log(LOG_LEVEL_ERR, "Not allowing connection from non-authorized IP '%s'", ipaddr);
        cf_closesocket(sd_accepted);
        return;
    }

    if (IsMatchItemIn(ctx, SV.attackerlist, MapAddress(ipaddr)))
    {
        Log(LOG_LEVEL_ERR, "Denying connection from non-authorized IP '%s'", ipaddr);
        cf_closesocket(sd_accepted);
        return;
    }

    if ((now = time((time_t *) NULL)) == -1)
       {
       now = 0;
       }

    PurgeOldConnections(&SV.connectionlist, now);

    if (!IsMatchItemIn(ctx, SV.multiconnlist, MapAddress(ipaddr)))
    {
        if (!ThreadLock(cft_count))
        {
            return;
        }

        if (IsItemIn(SV.connectionlist, MapAddress(ipaddr)))
        {
            ThreadUnlock(cft_count);
            Log(LOG_LEVEL_ERR, "Denying repeated connection from '%s'", ipaddr);
            cf_closesocket(sd_accepted);
            return;
        }

        ThreadUnlock(cft_count);
    }

    if (SV.logconns)
    {
        Log(LOG_LEVEL_INFO, "Accepting connection from %s", ipaddr);
    }
    else
    {
        Log(LOG_LEVEL_INFO, "Accepting connection from %s", ipaddr);
    }

    snprintf(intime, 63, "%d", (int) now);

    if (!ThreadLock(cft_count))
    {
        return;
    }

    PrependItem(&SV.connectionlist, MapAddress(ipaddr), intime);

    if (!ThreadUnlock(cft_count))
    {
       return;
    }

    SpawnConnection(ctx, sd_accepted, ipaddr);

}

/**********************************************************************/

void PurgeOldConnections(Item **list, time_t now)
   /* Some connections might not terminate properly. These should be cleaned
      every couple of hours. That should be enough to prevent spamming. */
{
    Item *ip;
    int then = 0;

    Log(LOG_LEVEL_DEBUG, "Purging Old Connections...");

    if (!ThreadLock(cft_count))
    {
        return;
    }

    if (list == NULL)
    {
        ThreadUnlock(cft_count);
        return;
    }

    Item *next;

    for (ip = *list; ip != NULL; ip = next)
    {
        sscanf(ip->classes, "%d", &then);

        next = ip->next;

        if (now > then + 7200)
        {
            Log(LOG_LEVEL_VERBOSE, "Purging IP address %s from connection list", ip->name);
            DeleteItem(list, ip);
        }
    }

    if (!ThreadUnlock(cft_count))
    {
        return;
    }

    Log(LOG_LEVEL_DEBUG, "Done purging old connections");
}

/*********************************************************************/

static void SpawnConnection(EvalContext *ctx, int sd_accepted, char *ipaddr)
{
    ServerConnectionState *conn;
    int ret;
    pthread_t tid;
    pthread_attr_t threadattrs;

    if ((conn = NewConn(ctx, sd_accepted)) == NULL)
    {
        return;
    }

    strncpy(conn->ipaddr, ipaddr, CF_MAX_IP_LEN - 1);

    Log(LOG_LEVEL_VERBOSE, "New connection...(from %s, sd %d)",
        conn->ipaddr, sd_accepted);
    Log(LOG_LEVEL_VERBOSE, "Spawning new thread...");

    ret = pthread_attr_init(&threadattrs);
    if (ret != 0)
    {
        Log(LOG_LEVEL_ERR,
            "SpawnConnection: Unable to initialize thread attributes (%s)",
            GetErrorStr());
        goto err2;
    }
    ret = pthread_attr_setdetachstate(&threadattrs, PTHREAD_CREATE_DETACHED);
    if (ret != 0)
    {
        Log(LOG_LEVEL_ERR,
            "SpawnConnection: Unable to set thread to detached state (%s).",
            GetErrorStr());
        goto err1;
    }
    ret = pthread_attr_setstacksize(&threadattrs, 1024 * 1024);
    if (ret != 0)
    {
        Log(LOG_LEVEL_WARNING,
            "SpawnConnection: Unable to set thread stack size (%s).",
            GetErrorStr());
        /* Continue with default thread stack size. */
    }

    ret = pthread_create(&tid, &threadattrs,
                         (void *(*)(void *)) HandleConnection, conn);
    if (ret != 0)
    {
        errno = ret;
        Log(LOG_LEVEL_ERR,
            "Unable to spawn worker thread. (pthread_create: %s)",
            GetErrorStr());
        goto err1;
    }

  err1:
    pthread_attr_destroy(&threadattrs);
  err2:
    if (ret != 0)
    {
        Log(LOG_LEVEL_WARNING, "Thread is being handled from main loop!");
        HandleConnection(conn);
    }
}

/*********************************************************************/

void DisableSendDelays(int sockfd)
{
    int yes = 1;

    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (void *) &yes, sizeof(yes)) == -1)
    {
        Log(LOG_LEVEL_INFO, "Unable to disable Nagle algorithm, expect performance problems. (setsockopt(TCP_NODELAY): %s)", GetErrorStr());
    }
}

/*********************************************************************/

static void *HandleConnection(ServerConnectionState *conn)
{
    char output[CF_BUFSIZE];

# ifdef HAVE_PTHREAD_SIGMASK
    sigset_t sigmask;

    sigemptyset(&sigmask);
    pthread_sigmask(SIG_BLOCK, &sigmask, NULL);
# endif

    if (!ThreadLock(cft_server_children))
    {
        DeleteConn(conn);
        return NULL;
    }

    ACTIVE_THREADS++;

    if (ACTIVE_THREADS >= CFD_MAXPROCESSES)
    {
        ACTIVE_THREADS--;

        if (TRIES++ > MAXTRIES) /* When to say we're hung / apoptosis threshold */
        {
            Log(LOG_LEVEL_ERR, "Server seems to be paralyzed. DOS attack? Committing apoptosis...");
            FatalError(conn->ctx, "Terminating");
        }

        if (!ThreadUnlock(cft_server_children))
        {
        }

        Log(LOG_LEVEL_ERR, "Too many threads (>=%d) -- increase server maxconnections?", CFD_MAXPROCESSES);
        snprintf(output, CF_BUFSIZE, "BAD: Server is currently too busy -- increase maxconnections or splaytime?");
        SendTransaction(&conn->conn_info, output, 0, CF_DONE);
        DeleteConn(conn);
        return NULL;
    }
    else
    {
        ThreadUnlock(cft_server_children);
    }

    TRIES = 0;                  /* As long as there is activity, we're not stuck */

    DisableSendDelays(conn->conn_info.sd);

    struct timeval tv = {
        .tv_sec = CONNTIMEOUT * 20,
    };

    SetReceiveTimeout(conn->conn_info.sd, &tv);

    /* TODO different busy loops for the two protocol versions */
    while (BusyWithConnection(conn->ctx, conn))
    {
    }

    Log(LOG_LEVEL_INFO, "Connection from %s is closed, terminating thread",
        conn->ipaddr);

    if (!ThreadLock(cft_server_children))
    {
        DeleteConn(conn);
        return NULL;
    }

    ACTIVE_THREADS--;

    if (!ThreadUnlock(cft_server_children))
    {
    }

    DeleteConn(conn);
    return NULL;
}

/*********************************************************************/

/*
 * We divide the protocol processing in two funcions:
 * 1. CFEngine_Classic_Protocol
 * 2. CFEngine_TLS_Protocol
 * This is done in order to simplify the handling of them.
 */

static int CFEngine_Classic_Protocol(EvalContext *ctx, ServerConnectionState *conn)
{
    time_t tloc, trem = 0;
    char recvbuffer[CF_BUFSIZE + CF_BUFEXT], sendbuffer[CF_BUFSIZE], check[CF_BUFSIZE];
    char filename[CF_BUFSIZE], buffer[CF_BUFSIZE], args[CF_BUFSIZE], out[CF_BUFSIZE];
    long time_no_see = 0;
    unsigned int len = 0;
    int drift, plainlen, received, encrypted = 0;
    ServerFileGetState get_args;
    Item *classes;

    memset(recvbuffer, 0, CF_BUFSIZE + CF_BUFEXT);
    memset(&get_args, 0, sizeof(get_args));

    received = ReceiveTransaction(&conn->conn_info, recvbuffer, NULL);
    if (received == -1 || received == 0)
    {
        return false;
    }

    if (strlen(recvbuffer) == 0)
    {
        Log(LOG_LEVEL_WARNING, "Got NULL transmission, skipping!");
        return true;
    }

    /* Don't process request if we're signalled to exit. */
    if (IsPendingTermination())
    {
        return false;
    }

    switch (GetCommand(recvbuffer))
    {
    case PROTOCOL_COMMAND_EXEC:
        memset(args, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "EXEC %255[^\n]", args);

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "Server refusal due to incorrect identity");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!AllowedUser(conn->username))
        {
            Log(LOG_LEVEL_INFO, "Server refusal due to non-allowed user");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!conn->rsa_auth)
        {
            Log(LOG_LEVEL_INFO, "Server refusal due to no RSA authentication");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!AccessControl(ctx, CommandArg0(CFRUNCOMMAND), conn, false))
        {
            Log(LOG_LEVEL_INFO, "Server refusal due to denied access to requested object");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!MatchClasses(ctx, conn))
        {
            Log(LOG_LEVEL_INFO, "Server refusal due to failed class/context match");
            Terminate(&conn->conn_info);
            return false;
        }

        DoExec(ctx, conn, args);
        Terminate(&conn->conn_info);
        return false;

    case PROTOCOL_COMMAND_VERSION:

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
        }

        snprintf(conn->output, CF_BUFSIZE, "OK: %s", Version());
        SendTransaction(&conn->conn_info, conn->output, 0, CF_DONE);
        return conn->id_verified;

    case PROTOCOL_COMMAND_AUTH_CLEAR:

        conn->id_verified = VerifyConnection(conn, (char *) (recvbuffer + strlen("CAUTH ")));

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
        }

        return conn->id_verified;       /* are we finished yet ? */

    case PROTOCOL_COMMAND_AUTH_SECURE:            /* This is where key agreement takes place */

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!AuthenticationDialogue(conn, recvbuffer, received))
        {
            Log(LOG_LEVEL_INFO, "Auth dialogue error");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        return true;

    case PROTOCOL_COMMAND_GET:

        memset(filename, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "GET %d %[^\n]", &(get_args.buf_size), filename);

        if ((get_args.buf_size < 0) || (get_args.buf_size > CF_BUFSIZE))
        {
            Log(LOG_LEVEL_INFO, "GET buffer out of bounds");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!AccessControl(ctx, filename, conn, false))
        {
            Log(LOG_LEVEL_INFO, "Access denied to get object");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        memset(sendbuffer, 0, CF_BUFSIZE);

        if (get_args.buf_size >= CF_BUFSIZE)
        {
            get_args.buf_size = 2048;
        }

        get_args.connect = conn;
        get_args.encrypt = false;
        get_args.replybuff = sendbuffer;
        get_args.replyfile = filename;

        CfGetFile(&get_args);

        return true;

    case PROTOCOL_COMMAND_GET_SECURE:

        memset(buffer, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "SGET %u %d", &len, &(get_args.buf_size));

        if (received != len + CF_PROTO_OFFSET)
        {
            Log(LOG_LEVEL_VERBOSE, "Protocol error SGET");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        plainlen = DecryptString(conn->encryption_type, recvbuffer + CF_PROTO_OFFSET, buffer, conn->session_key, len);

        cfscanf(buffer, strlen("GET"), strlen("dummykey"), check, sendbuffer, filename);

        if (strcmp(check, "GET") != 0)
        {
            Log(LOG_LEVEL_INFO, "SGET/GET problem");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        if ((get_args.buf_size < 0) || (get_args.buf_size > 8192))
        {
            Log(LOG_LEVEL_INFO, "SGET bounding error");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (get_args.buf_size >= CF_BUFSIZE)
        {
            get_args.buf_size = 2048;
        }

        Log(LOG_LEVEL_DEBUG, "Confirm decryption, and thus validity of caller");
        Log(LOG_LEVEL_DEBUG, "SGET '%s' with blocksize %d", filename, get_args.buf_size);

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!AccessControl(ctx, filename, conn, true))
        {
            Log(LOG_LEVEL_INFO, "Access control error");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        memset(sendbuffer, 0, CF_BUFSIZE);

        get_args.connect = conn;
        get_args.encrypt = true;
        get_args.replybuff = sendbuffer;
        get_args.replyfile = filename;

        CfEncryptGetFile(&get_args);
        return true;

    case PROTOCOL_COMMAND_OPENDIR_SECURE:

        memset(buffer, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "SOPENDIR %u", &len);

        if ((len >= sizeof(out)) || (received != (len + CF_PROTO_OFFSET)))
        {
            Log(LOG_LEVEL_VERBOSE, "Protocol error OPENDIR: %d", len);
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (conn->session_key == NULL)
        {
            Log(LOG_LEVEL_INFO, "No session key");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        memcpy(out, recvbuffer + CF_PROTO_OFFSET, len);

        plainlen = DecryptString(conn->encryption_type, out, recvbuffer, conn->session_key, len);

        if (strncmp(recvbuffer, "OPENDIR", 7) != 0)
        {
            Log(LOG_LEVEL_INFO, "Opendir failed to decrypt");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        memset(filename, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "OPENDIR %[^\n]", filename);

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!AccessControl(ctx, filename, conn, true))        /* opendir don't care about privacy */
        {
            Log(LOG_LEVEL_INFO, "Access error");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        CfSecOpenDirectory(conn, sendbuffer, filename);
        return true;

    case PROTOCOL_COMMAND_OPENDIR:

        memset(filename, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "OPENDIR %[^\n]", filename);

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!AccessControl(ctx, filename, conn, true))        /* opendir don't care about privacy */
        {
            Log(LOG_LEVEL_INFO, "DIR access error");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        CfOpenDirectory(conn, sendbuffer, filename);
        return true;

    case PROTOCOL_COMMAND_SYNC_SECURE:

        memset(buffer, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "SSYNCH %u", &len);

        if ((len >= sizeof(out)) || (received != (len + CF_PROTO_OFFSET)))
        {
            Log(LOG_LEVEL_VERBOSE, "Protocol error SSYNCH: %d", len);
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (conn->session_key == NULL)
        {
            Log(LOG_LEVEL_INFO, "Bad session key");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        memcpy(out, recvbuffer + CF_PROTO_OFFSET, len);

        plainlen = DecryptString(conn->encryption_type, out, recvbuffer, conn->session_key, len);

        if (plainlen < 0)
        {
            DebugBinOut((char *) conn->session_key, 32, "Session key");
            Log(LOG_LEVEL_ERR, "Bad decrypt (%d)", len);
        }

        if (strncmp(recvbuffer, "SYNCH", 5) != 0)
        {
            Log(LOG_LEVEL_INFO, "No synch");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        /* roll through, no break */

    case PROTOCOL_COMMAND_SYNC:

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        memset(filename, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "SYNCH %ld STAT %[^\n]", &time_no_see, filename);

        trem = (time_t) time_no_see;

        if ((time_no_see == 0) || (filename[0] == '\0'))
        {
            break;
        }

        if ((tloc = time((time_t *) NULL)) == -1)
        {
            sprintf(conn->output, "Couldn't read system clock\n");
            Log(LOG_LEVEL_INFO, "Couldn't read system clock. (time: %s)", GetErrorStr());
            SendTransaction(&conn->conn_info, "BAD: clocks out of synch", 0, CF_DONE);
            return true;
        }

        drift = (int) (tloc - trem);

        if (!AccessControl(ctx, filename, conn, true))
        {
            Log(LOG_LEVEL_INFO, "Access control in sync");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        if (DENYBADCLOCKS && (drift * drift > CLOCK_DRIFT * CLOCK_DRIFT))
        {
            snprintf(conn->output, CF_BUFSIZE - 1, "BAD: Clocks are too far unsynchronized %ld/%ld\n", (long) tloc,
                     (long) trem);
            SendTransaction(&conn->conn_info, conn->output, 0, CF_DONE);
            return true;
        }
        else
        {
            Log(LOG_LEVEL_DEBUG, "Clocks were off by %ld", (long) tloc - (long) trem);
            StatFile(conn, sendbuffer, filename);
        }

        return true;

    case PROTOCOL_COMMAND_MD5_SECURE:

        sscanf(recvbuffer, "SMD5 %u", &len);

        if ((len >= sizeof(out)) || (received != (len + CF_PROTO_OFFSET)))
        {
            Log(LOG_LEVEL_INFO, "Decryption error");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        memcpy(out, recvbuffer + CF_PROTO_OFFSET, len);
        plainlen = DecryptString(conn->encryption_type, out, recvbuffer, conn->session_key, len);

        if (strncmp(recvbuffer, "MD5", 3) != 0)
        {
            Log(LOG_LEVEL_INFO, "MD5 protocol error");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        /* roll through, no break */

    case PROTOCOL_COMMAND_MD5:

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        CompareLocalHash(conn, sendbuffer, recvbuffer);
        return true;

    case PROTOCOL_COMMAND_VAR_SECURE:

        sscanf(recvbuffer, "SVAR %u", &len);

        if ((len >= sizeof(out)) || (received != (len + CF_PROTO_OFFSET)))
        {
            Log(LOG_LEVEL_INFO, "Decrypt error SVAR");
            RefuseAccess(conn, 0, "decrypt error SVAR");
            return true;
        }

        memcpy(out, recvbuffer + CF_PROTO_OFFSET, len);
        plainlen = DecryptString(conn->encryption_type, out, recvbuffer, conn->session_key, len);
        encrypted = true;

        if (strncmp(recvbuffer, "VAR", 3) != 0)
        {
            Log(LOG_LEVEL_INFO, "VAR protocol defect");
            RefuseAccess(conn, 0, "decryption failure");
            return false;
        }

        /* roll through, no break */

    case PROTOCOL_COMMAND_VAR:

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        if (!LiteralAccessControl(ctx, recvbuffer, conn, encrypted))
        {
            Log(LOG_LEVEL_INFO, "Literal access failure");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        GetServerLiteral(ctx, conn, sendbuffer, recvbuffer, encrypted);
        return true;

    case PROTOCOL_COMMAND_CONTEXT_SECURE:

        sscanf(recvbuffer, "SCONTEXT %u", &len);

        if ((len >= sizeof(out)) || (received != (len + CF_PROTO_OFFSET)))
        {
            Log(LOG_LEVEL_INFO, "Decrypt error SCONTEXT, len,received = %d,%d", len, received);
            RefuseAccess(conn, 0, "decrypt error SCONTEXT");
            return true;
        }

        memcpy(out, recvbuffer + CF_PROTO_OFFSET, len);
        plainlen = DecryptString(conn->encryption_type, out, recvbuffer, conn->session_key, len);
        encrypted = true;

        if (strncmp(recvbuffer, "CONTEXT", 7) != 0)
        {
            Log(LOG_LEVEL_INFO, "CONTEXT protocol defect...");
            RefuseAccess(conn, 0, "Decryption failed?");
            return false;
        }

        /* roll through, no break */

    case PROTOCOL_COMMAND_CONTEXT:

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, "Context probe");
            return true;
        }

        if ((classes = ContextAccessControl(ctx, recvbuffer, conn, encrypted)) == NULL)
        {
            Log(LOG_LEVEL_INFO, "Context access failure on %s", recvbuffer);
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        ReplyServerContext(conn, encrypted, classes);
        return true;

    case PROTOCOL_COMMAND_QUERY_SECURE:

        sscanf(recvbuffer, "SQUERY %u", &len);

        if ((len >= sizeof(out)) || (received != (len + CF_PROTO_OFFSET)))
        {
            Log(LOG_LEVEL_INFO, "Decrypt error SQUERY");
            RefuseAccess(conn, 0, "decrypt error SQUERY");
            return true;
        }

        memcpy(out, recvbuffer + CF_PROTO_OFFSET, len);
        plainlen = DecryptString(conn->encryption_type, out, recvbuffer, conn->session_key, len);

        if (strncmp(recvbuffer, "QUERY", 5) != 0)
        {
            Log(LOG_LEVEL_INFO, "QUERY protocol defect");
            RefuseAccess(conn, 0, "decryption failure");
            return false;
        }

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        if (!LiteralAccessControl(ctx, recvbuffer, conn, true))
        {
            Log(LOG_LEVEL_INFO, "Query access failure");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (GetServerQuery(conn, recvbuffer))
        {
            return true;
        }

        break;
    case PROTOCOL_COMMAND_STARTTLS:
        /* TODO move out of ClassicProtocol protocol loop */
        if (DoStartTLS(conn) < 0)
        {
            Log(LOG_LEVEL_ERR, "Could not start TLS session as requested by client");
            return false;
        }
        else
        {
            /* TODO we need the public key hash in lastseen to be specified by both username+ip+hostname! */
            /*
             * TODO agent identity should be *KEY* and each on each connection a new username+ip+hostname
             *      entry should be recorded in lastseen.
             * By default a specific *KEY* is denied access if any of the other properties have changed.
             * However they can be set as "flexible", e.g. flexible-IP would allow address to change.
             */
            /* TODO we need the username inside the certificate we receive, so that we extract and check with no CAUTH.  */

            int ret = TLSVerifyPeer(conn->conn_info.ssl, conn->ipaddr, "root");
            if (ret == 1)
            {
                Log(LOG_LEVEL_INFO, "Client is TRUSTED, public key MATCHES stored one.");
                /* skipping CAUTH */
                conn->id_verified = 1;
                /* skipping SAUTH, allow access to read-only files too */
                conn->rsa_auth = 1;
                return true;
            }
            else if (ret == 0)
            {
                Log(LOG_LEVEL_ERR, "Client's public key DIFFERS from the one stored!");
                Log(LOG_LEVEL_ERR, "TRUST FAILED, WARNING: possible MAN IN THE MIDDLE attack!");
                Log(LOG_LEVEL_ERR, "Open server's ACL if you really want to start trusting this new key.");
                return false;
            }
            return false;
        }
        break;

    case PROTOCOL_COMMAND_CALL_ME_BACK:

        sscanf(recvbuffer, "SCALLBACK %u", &len);

        if ((len >= sizeof(out)) || (received != (len + CF_PROTO_OFFSET)))
        {
            Log(LOG_LEVEL_INFO, "Decrypt error CALL_ME_BACK");
            RefuseAccess(conn, 0, "decrypt error CALL_ME_BACK");
            return true;
        }

        memcpy(out, recvbuffer + CF_PROTO_OFFSET, len);
        plainlen = DecryptString(conn->encryption_type, out, recvbuffer, conn->session_key, len);

        if (strncmp(recvbuffer, "CALL_ME_BACK collect_calls", strlen("CALL_ME_BACK collect_calls")) != 0)
        {
            Log(LOG_LEVEL_INFO, "CALL_ME_BACK protocol defect");
            RefuseAccess(conn, 0, "decryption failure");
            return false;
        }

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        if (!LiteralAccessControl(ctx, recvbuffer, conn, true))
        {
            Log(LOG_LEVEL_INFO, "Query access failure");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (ReceiveCollectCall(conn))
        {
            return true;
        }

    case PROTOCOL_COMMAND_AUTH:
    case PROTOCOL_COMMAND_CONTEXTS:
    case PROTOCOL_COMMAND_BAD:
        ProgrammingError("Unexpected protocol command");
    }

    sprintf(sendbuffer, "BAD: Request denied\n");
    SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
    Log(LOG_LEVEL_INFO, "Closing connection, due to request: '%s'", recvbuffer);
    return false;
}

static int CFEngine_TLS_Protocol(EvalContext *ctx, ServerConnectionState *conn)
{
    time_t tloc, trem = 0;
    char recvbuffer[CF_BUFSIZE + CF_BUFEXT], sendbuffer[CF_BUFSIZE], check[CF_BUFSIZE];
    char filename[CF_BUFSIZE], buffer[CF_BUFSIZE], args[CF_BUFSIZE], out[CF_BUFSIZE];
    long time_no_see = 0;
    unsigned int len = 0;
    int drift, plainlen, received, encrypted = 0;
    ServerFileGetState get_args;
    Item *classes;

    memset(recvbuffer, 0, CF_BUFSIZE + CF_BUFEXT);
    memset(&get_args, 0, sizeof(get_args));

    received = ReceiveTransaction(&conn->conn_info, recvbuffer, NULL);
    if (received == -1 || received == 0)
    {
        return false;
    }

    if (strlen(recvbuffer) == 0)
    {
        Log(LOG_LEVEL_WARNING, "Got NULL transmission, skipping!");
        return true;
    }

    /* Don't process request if we're signalled to exit. */
    if (IsPendingTermination())
    {
        return false;
    }

    switch (GetCommand(recvbuffer))
    {
    case PROTOCOL_COMMAND_EXEC:
        memset(args, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "EXEC %255[^\n]", args);

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "Server refusal due to incorrect identity");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!AllowedUser(conn->username))
        {
            Log(LOG_LEVEL_INFO, "Server refusal due to non-allowed user");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!conn->rsa_auth)
        {
            Log(LOG_LEVEL_INFO, "Server refusal due to no RSA authentication");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!AccessControl(ctx, CommandArg0(CFRUNCOMMAND), conn, false))
        {
            Log(LOG_LEVEL_INFO, "Server refusal due to denied access to requested object");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!MatchClasses(ctx, conn))
        {
            Log(LOG_LEVEL_INFO, "Server refusal due to failed class/context match");
            Terminate(&conn->conn_info);
            return false;
        }

        DoExec(ctx, conn, args);
        Terminate(&conn->conn_info);
        return false;

    case PROTOCOL_COMMAND_VERSION:

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
        }

        snprintf(conn->output, CF_BUFSIZE, "OK: %s", Version());
        SendTransaction(&conn->conn_info, conn->output, 0, CF_DONE);
        return conn->id_verified;

    case PROTOCOL_COMMAND_GET:

        memset(filename, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "GET %d %[^\n]", &(get_args.buf_size), filename);

        if ((get_args.buf_size < 0) || (get_args.buf_size > CF_BUFSIZE))
        {
            Log(LOG_LEVEL_INFO, "GET buffer out of bounds");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!AccessControl(ctx, filename, conn, false))
        {
            Log(LOG_LEVEL_INFO, "Access denied to get object");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        memset(sendbuffer, 0, CF_BUFSIZE);

        if (get_args.buf_size >= CF_BUFSIZE)
        {
            get_args.buf_size = 2048;
        }

        get_args.connect = conn;
        get_args.encrypt = false;
        get_args.replybuff = sendbuffer;
        get_args.replyfile = filename;

        CfGetFile(&get_args);

        return true;

    case PROTOCOL_COMMAND_GET_SECURE:

        memset(buffer, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "SGET %u %d", &len, &(get_args.buf_size));

        if (received != len + CF_PROTO_OFFSET)
        {
            Log(LOG_LEVEL_VERBOSE, "Protocol error SGET");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        plainlen = DecryptString(conn->encryption_type, recvbuffer + CF_PROTO_OFFSET, buffer, conn->session_key, len);

        cfscanf(buffer, strlen("GET"), strlen("dummykey"), check, sendbuffer, filename);

        if (strcmp(check, "GET") != 0)
        {
            Log(LOG_LEVEL_INFO, "SGET/GET problem");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        if ((get_args.buf_size < 0) || (get_args.buf_size > 8192))
        {
            Log(LOG_LEVEL_INFO, "SGET bounding error");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (get_args.buf_size >= CF_BUFSIZE)
        {
            get_args.buf_size = 2048;
        }

        Log(LOG_LEVEL_DEBUG, "Confirm decryption, and thus validity of caller");
        Log(LOG_LEVEL_DEBUG, "SGET '%s' with blocksize %d", filename, get_args.buf_size);

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!AccessControl(ctx, filename, conn, true))
        {
            Log(LOG_LEVEL_INFO, "Access control error");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        memset(sendbuffer, 0, CF_BUFSIZE);

        get_args.connect = conn;
        get_args.encrypt = true;
        get_args.replybuff = sendbuffer;
        get_args.replyfile = filename;

        CfEncryptGetFile(&get_args);
        return true;

    case PROTOCOL_COMMAND_OPENDIR_SECURE:

        memset(buffer, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "SOPENDIR %u", &len);

        if ((len >= sizeof(out)) || (received != (len + CF_PROTO_OFFSET)))
        {
            Log(LOG_LEVEL_VERBOSE, "Protocol error OPENDIR: %d", len);
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (conn->session_key == NULL)
        {
            Log(LOG_LEVEL_INFO, "No session key");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        memcpy(out, recvbuffer + CF_PROTO_OFFSET, len);

        plainlen = DecryptString(conn->encryption_type, out, recvbuffer, conn->session_key, len);

        if (strncmp(recvbuffer, "OPENDIR", 7) != 0)
        {
            Log(LOG_LEVEL_INFO, "Opendir failed to decrypt");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        memset(filename, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "OPENDIR %[^\n]", filename);

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!AccessControl(ctx, filename, conn, true))        /* opendir don't care about privacy */
        {
            Log(LOG_LEVEL_INFO, "Access error");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        CfSecOpenDirectory(conn, sendbuffer, filename);
        return true;

    case PROTOCOL_COMMAND_OPENDIR:

        memset(filename, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "OPENDIR %[^\n]", filename);

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (!AccessControl(ctx, filename, conn, true))        /* opendir don't care about privacy */
        {
            Log(LOG_LEVEL_INFO, "DIR access error");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        CfOpenDirectory(conn, sendbuffer, filename);
        return true;

    case PROTOCOL_COMMAND_SYNC_SECURE:

        memset(buffer, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "SSYNCH %u", &len);

        if ((len >= sizeof(out)) || (received != (len + CF_PROTO_OFFSET)))
        {
            Log(LOG_LEVEL_VERBOSE, "Protocol error SSYNCH: %d", len);
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (conn->session_key == NULL)
        {
            Log(LOG_LEVEL_INFO, "Bad session key");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        memcpy(out, recvbuffer + CF_PROTO_OFFSET, len);

        plainlen = DecryptString(conn->encryption_type, out, recvbuffer, conn->session_key, len);

        if (plainlen < 0)
        {
            DebugBinOut((char *) conn->session_key, 32, "Session key");
            Log(LOG_LEVEL_ERR, "Bad decrypt (%d)", len);
        }

        if (strncmp(recvbuffer, "SYNCH", 5) != 0)
        {
            Log(LOG_LEVEL_INFO, "No synch");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        /* roll through, no break */

    case PROTOCOL_COMMAND_SYNC:

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        memset(filename, 0, CF_BUFSIZE);
        sscanf(recvbuffer, "SYNCH %ld STAT %[^\n]", &time_no_see, filename);

        trem = (time_t) time_no_see;

        if ((time_no_see == 0) || (filename[0] == '\0'))
        {
            break;
        }

        if ((tloc = time((time_t *) NULL)) == -1)
        {
            sprintf(conn->output, "Couldn't read system clock\n");
            Log(LOG_LEVEL_INFO, "Couldn't read system clock. (time: %s)", GetErrorStr());
            SendTransaction(&conn->conn_info, "BAD: clocks out of synch", 0, CF_DONE);
            return true;
        }

        drift = (int) (tloc - trem);

        if (!AccessControl(ctx, filename, conn, true))
        {
            Log(LOG_LEVEL_INFO, "Access control in sync");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        if (DENYBADCLOCKS && (drift * drift > CLOCK_DRIFT * CLOCK_DRIFT))
        {
            snprintf(conn->output, CF_BUFSIZE - 1, "BAD: Clocks are too far unsynchronized %ld/%ld\n", (long) tloc,
                     (long) trem);
            SendTransaction(&conn->conn_info, conn->output, 0, CF_DONE);
            return true;
        }
        else
        {
            Log(LOG_LEVEL_DEBUG, "Clocks were off by %ld", (long) tloc - (long) trem);
            StatFile(conn, sendbuffer, filename);
        }

        return true;

    case PROTOCOL_COMMAND_MD5_SECURE:

        sscanf(recvbuffer, "SMD5 %u", &len);

        if ((len >= sizeof(out)) || (received != (len + CF_PROTO_OFFSET)))
        {
            Log(LOG_LEVEL_INFO, "Decryption error");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        memcpy(out, recvbuffer + CF_PROTO_OFFSET, len);
        plainlen = DecryptString(conn->encryption_type, out, recvbuffer, conn->session_key, len);

        if (strncmp(recvbuffer, "MD5", 3) != 0)
        {
            Log(LOG_LEVEL_INFO, "MD5 protocol error");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        /* roll through, no break */

    case PROTOCOL_COMMAND_MD5:

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        CompareLocalHash(conn, sendbuffer, recvbuffer);
        return true;

    case PROTOCOL_COMMAND_VAR_SECURE:

        sscanf(recvbuffer, "SVAR %u", &len);

        if ((len >= sizeof(out)) || (received != (len + CF_PROTO_OFFSET)))
        {
            Log(LOG_LEVEL_INFO, "Decrypt error SVAR");
            RefuseAccess(conn, 0, "decrypt error SVAR");
            return true;
        }

        memcpy(out, recvbuffer + CF_PROTO_OFFSET, len);
        plainlen = DecryptString(conn->encryption_type, out, recvbuffer, conn->session_key, len);
        encrypted = true;

        if (strncmp(recvbuffer, "VAR", 3) != 0)
        {
            Log(LOG_LEVEL_INFO, "VAR protocol defect");
            RefuseAccess(conn, 0, "decryption failure");
            return false;
        }

        /* roll through, no break */

    case PROTOCOL_COMMAND_VAR:

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        if (!LiteralAccessControl(ctx, recvbuffer, conn, encrypted))
        {
            Log(LOG_LEVEL_INFO, "Literal access failure");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        GetServerLiteral(ctx, conn, sendbuffer, recvbuffer, encrypted);
        return true;

    case PROTOCOL_COMMAND_CONTEXT_SECURE:

        sscanf(recvbuffer, "SCONTEXT %u", &len);

        if ((len >= sizeof(out)) || (received != (len + CF_PROTO_OFFSET)))
        {
            Log(LOG_LEVEL_INFO, "Decrypt error SCONTEXT, len,received = %d,%d", len, received);
            RefuseAccess(conn, 0, "decrypt error SCONTEXT");
            return true;
        }

        memcpy(out, recvbuffer + CF_PROTO_OFFSET, len);
        plainlen = DecryptString(conn->encryption_type, out, recvbuffer, conn->session_key, len);
        encrypted = true;

        if (strncmp(recvbuffer, "CONTEXT", 7) != 0)
        {
            Log(LOG_LEVEL_INFO, "CONTEXT protocol defect...");
            RefuseAccess(conn, 0, "Decryption failed?");
            return false;
        }

        /* roll through, no break */

    case PROTOCOL_COMMAND_CONTEXT:

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, "Context probe");
            return true;
        }

        if ((classes = ContextAccessControl(ctx, recvbuffer, conn, encrypted)) == NULL)
        {
            Log(LOG_LEVEL_INFO, "Context access failure on %s", recvbuffer);
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        ReplyServerContext(conn, encrypted, classes);
        return true;

    case PROTOCOL_COMMAND_QUERY_SECURE:

        sscanf(recvbuffer, "SQUERY %u", &len);

        if ((len >= sizeof(out)) || (received != (len + CF_PROTO_OFFSET)))
        {
            Log(LOG_LEVEL_INFO, "Decrypt error SQUERY");
            RefuseAccess(conn, 0, "decrypt error SQUERY");
            return true;
        }

        memcpy(out, recvbuffer + CF_PROTO_OFFSET, len);
        plainlen = DecryptString(conn->encryption_type, out, recvbuffer, conn->session_key, len);

        if (strncmp(recvbuffer, "QUERY", 5) != 0)
        {
            Log(LOG_LEVEL_INFO, "QUERY protocol defect");
            RefuseAccess(conn, 0, "decryption failure");
            return false;
        }

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        if (!LiteralAccessControl(ctx, recvbuffer, conn, true))
        {
            Log(LOG_LEVEL_INFO, "Query access failure");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }

        if (GetServerQuery(conn, recvbuffer))
        {
            return true;
        }

        break;

    case PROTOCOL_COMMAND_CALL_ME_BACK:

        sscanf(recvbuffer, "SCALLBACK %u", &len);

        if ((len >= sizeof(out)) || (received != (len + CF_PROTO_OFFSET)))
        {
            Log(LOG_LEVEL_INFO, "Decrypt error CALL_ME_BACK");
            RefuseAccess(conn, 0, "decrypt error CALL_ME_BACK");
            return true;
        }

        memcpy(out, recvbuffer + CF_PROTO_OFFSET, len);
        plainlen = DecryptString(conn->encryption_type, out, recvbuffer, conn->session_key, len);

        if (strncmp(recvbuffer, "CALL_ME_BACK collect_calls", strlen("CALL_ME_BACK collect_calls")) != 0)
        {
            Log(LOG_LEVEL_INFO, "CALL_ME_BACK protocol defect");
            RefuseAccess(conn, 0, "decryption failure");
            return false;
        }

        if (!conn->id_verified)
        {
            Log(LOG_LEVEL_INFO, "ID not verified");
            RefuseAccess(conn, 0, recvbuffer);
            return true;
        }

        if (!LiteralAccessControl(ctx, recvbuffer, conn, true))
        {
            Log(LOG_LEVEL_INFO, "Query access failure");
            RefuseAccess(conn, 0, recvbuffer);
            return false;
        }
        return ReceiveCollectCall(conn);

    case PROTOCOL_COMMAND_AUTH:
    case PROTOCOL_COMMAND_CONTEXTS:
    case PROTOCOL_COMMAND_BAD:
    default:
        Log(LOG_LEVEL_WARNING, "Unexpected protocol command");
    }

    sprintf(sendbuffer, "BAD: Request denied\n");
    SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
    Log(LOG_LEVEL_INFO, "Closing connection, due to request: '%s'", recvbuffer);
    return false;
    return 0;
}

static int BusyWithConnection(EvalContext *ctx, ServerConnectionState *conn)
  /* This is the protocol section. Here we must   */
  /* check that the incoming data are sensible    */
  /* and extract the information from the message */
{
    if (conn->conn_info.type == CF_PROTOCOL_CLASSIC)
    {
        return CFEngine_Classic_Protocol(ctx, conn);
    }
    /*
     * The reason we don't have an else clause it is because
     * we can change from Classic to TLS, therefore if we ask
     * for the type of connection after we are done with the
     * classic connection we avoid a more complicated logic.
     */
    if (conn->conn_info.type == CF_PROTOCOL_TLS)
    {
        return CFEngine_TLS_Protocol(ctx, conn);
    }

    UnexpectedError("BusyWithConnection: ProtocolVersion %d!",
                    conn->conn_info.type);
    return false;
}

/**************************************************************/
/* Level 4                                                    */
/**************************************************************/

static int MatchClasses(EvalContext *ctx, ServerConnectionState *conn)
{
    char recvbuffer[CF_BUFSIZE];
    Item *classlist = NULL, *ip;
    int count = 0;

    while (true && (count < 10))        /* arbitrary check to avoid infinite loop, DoS attack */
    {
        count++;

        if (ReceiveTransaction(&conn->conn_info, recvbuffer, NULL) == -1)
        {
            Log(LOG_LEVEL_VERBOSE, "Unable to read data from network. (ReceiveTransaction: %s)", GetErrorStr());
            return false;
        }

        Log(LOG_LEVEL_DEBUG, "Got class buffer '%s'", recvbuffer);

        if (strncmp(recvbuffer, CFD_TERMINATOR, strlen(CFD_TERMINATOR)) == 0)
        {
            if (count == 1)
            {
                Log(LOG_LEVEL_DEBUG, "No classes were sent, assuming no restrictions...");
                return true;
            }

            break;
        }

        classlist = SplitStringAsItemList(recvbuffer, ' ');

        for (ip = classlist; ip != NULL; ip = ip->next)
        {
            Log(LOG_LEVEL_VERBOSE, "Checking whether class %s can be identified as me...", ip->name);

            if (IsDefinedClass(ctx, ip->name, NULL))
            {
                Log(LOG_LEVEL_DEBUG, "Class '%s' matched, accepting...", ip->name);
                DeleteItemList(classlist);
                return true;
            }

            if (EvalContextHeapMatchCountSoft(ctx, ip->name) > 0)
            {
                Log(LOG_LEVEL_DEBUG, "Class matched regular expression '%s', accepting...", ip->name);
                DeleteItemList(classlist);
                return true;
            }

            if (EvalContextHeapMatchCountHard(ctx, ip->name))
            {
                Log(LOG_LEVEL_DEBUG, "Class matched regular expression '%s', accepting...", ip->name);
                DeleteItemList(classlist);
                return true;
            }

            if (strncmp(ip->name, CFD_TERMINATOR, strlen(CFD_TERMINATOR)) == 0)
            {
                Log(LOG_LEVEL_VERBOSE, "No classes matched, rejecting....");
                ReplyNothing(conn);
                DeleteItemList(classlist);
                return false;
            }
        }
    }

    ReplyNothing(conn);
    Log(LOG_LEVEL_VERBOSE, "No classes matched, rejecting....");
    DeleteItemList(classlist);
    return false;
}

/******************************************************************/

static void DoExec(EvalContext *ctx, ServerConnectionState *conn, char *args)
{
    char ebuff[CF_EXPANDSIZE], line[CF_BUFSIZE], *sp;
    int print = false, i;
    FILE *pp;

    if ((CFSTARTTIME = time((time_t *) NULL)) == -1)
    {
        Log(LOG_LEVEL_ERR, "Couldn't read system clock. (time: %s)", GetErrorStr());
    }

    if (strlen(CFRUNCOMMAND) == 0)
    {
        Log(LOG_LEVEL_VERBOSE, "cf-serverd exec request: no cfruncommand defined");
        char sendbuffer[CF_BUFSIZE];
        strlcpy(sendbuffer, "Exec request: no cfruncommand defined\n", CF_BUFSIZE);
        SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
        return;
    }

    Log(LOG_LEVEL_VERBOSE, "Examining command string '%s'", args);

    for (sp = args; *sp != '\0'; sp++)  /* Blank out -K -f */
    {
        if ((*sp == ';') || (*sp == '&') || (*sp == '|'))
        {
            char sendbuffer[CF_BUFSIZE];
            snprintf(sendbuffer, CF_BUFSIZE, "You are not authorized to activate these classes/roles on host %s\n", VFQNAME);
            SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
            return;
        }

        if ((OptionFound(args, sp, "-K")) || (OptionFound(args, sp, "-f")))
        {
            *sp = ' ';
            *(sp + 1) = ' ';
        }
        else if (OptionFound(args, sp, "--no-lock"))
        {
            for (i = 0; i < strlen("--no-lock"); i++)
            {
                *(sp + i) = ' ';
            }
        }
        else if (OptionFound(args, sp, "--file"))
        {
            for (i = 0; i < strlen("--file"); i++)
            {
                *(sp + i) = ' ';
            }
        }
        else if ((OptionFound(args, sp, "--define")) || (OptionFound(args, sp, "-D")))
        {
            Log(LOG_LEVEL_VERBOSE, "Attempt to activate a predefined role..");

            if (!AuthorizeRoles(ctx, conn, sp))
            {
                char sendbuffer[CF_BUFSIZE];
                snprintf(sendbuffer, CF_BUFSIZE, "You are not authorized to activate these classes/roles on host %s\n", VFQNAME);
                SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
                return;
            }
        }
    }

    snprintf(ebuff, CF_BUFSIZE, "%s --inform", CFRUNCOMMAND);

    if (strlen(ebuff) + strlen(args) + 6 > CF_BUFSIZE)
    {
        char sendbuffer[CF_BUFSIZE];
        snprintf(sendbuffer, CF_BUFSIZE, "Command line too long with args: %s\n", ebuff);
        SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
        return;
    }
    else
    {
        if ((args != NULL) && (strlen(args) > 0))
        {
            char sendbuffer[CF_BUFSIZE];
            strcat(ebuff, " ");
            strncat(ebuff, args, CF_BUFSIZE - strlen(ebuff));
            snprintf(sendbuffer, CF_BUFSIZE, "cf-serverd Executing %s\n", ebuff);
            SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
        }
    }

    Log(LOG_LEVEL_INFO, "Executing command %s", ebuff);

    if ((pp = cf_popen_sh(ebuff, "r")) == NULL)
    {
        Log(LOG_LEVEL_ERR, "Couldn't open pipe to command '%s'. (pipe: %s)", ebuff, GetErrorStr());
        char sendbuffer[CF_BUFSIZE];
        snprintf(sendbuffer, CF_BUFSIZE, "Unable to run %s\n", ebuff);
        SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
        return;
    }

    for (;;)
    {
        ssize_t res = CfReadLine(line, CF_BUFSIZE, pp);

        if (res == 0)
        {
            break;
        }

        if (res == -1)
        {
            fflush(pp); /* FIXME: is it necessary? */
            break;
        }

        print = false;

        for (sp = line; *sp != '\0'; sp++)
        {
            if (!isspace((int) *sp))
            {
                print = true;
                break;
            }
        }

        if (print)
        {
            char sendbuffer[CF_BUFSIZE];
            snprintf(sendbuffer, CF_BUFSIZE, "%s\n", line);
            if (SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE) == -1)
            {
                Log(LOG_LEVEL_ERR, "Sending failed, aborting. (send: %s)", GetErrorStr());
                break;
            }
        }
    }

    cf_pclose(pp);
}

/**************************************************************/
ProtocolCommand GetCommand(char *str)
{
    int i;
    for (i = 0; PROTOCOL[i] != NULL; i++)
    {
        int cmdlen = strlen(PROTOCOL[i]);
        if ((strncmp(str, PROTOCOL[i], cmdlen) == 0) &&
            (str[cmdlen] == ' ' || str[cmdlen] == '\0'))
        {
            return i;
        }
    }
    assert (i == PROTOCOL_COMMAND_BAD);
    return i;
}

/*********************************************************************/

static int VerifyConnection(ServerConnectionState *conn, char buf[CF_BUFSIZE])
 /* Try reverse DNS lookup
    and RFC931 username lookup to check the authenticity. */
{
    char ipstring[CF_MAXVARSIZE], fqname[CF_MAXVARSIZE], username[CF_MAXVARSIZE];
    char dns_assert[CF_MAXVARSIZE], ip_assert[CF_MAXVARSIZE];
    int matched = false;

    Log(LOG_LEVEL_DEBUG, "Connecting host identifies itself as '%s'", buf);

    memset(ipstring, 0, CF_MAXVARSIZE);
    memset(fqname, 0, CF_MAXVARSIZE);
    memset(username, 0, CF_MAXVARSIZE);

    sscanf(buf, "%255s %255s %255s", ipstring, fqname, username);

    Log(LOG_LEVEL_DEBUG, "(ipstring=[%s],fqname=[%s],username=[%s],socket=[%s])",
            ipstring, fqname, username, conn->ipaddr);

    strlcpy(dns_assert, fqname, CF_MAXVARSIZE);
    ToLowerStrInplace(dns_assert);

    strncpy(ip_assert, ipstring, CF_MAXVARSIZE - 1);

/* It only makes sense to check DNS by reverse lookup if the key had to be
   accepted on trust. Once we have a positive key ID, the IP address is
   irrelevant fr authentication...
   We can save a lot of time by not looking this up ... */

    if ((conn->trust == false) ||
        (IsMatchItemIn(conn->ctx, SV.skipverify, MapAddress(conn->ipaddr))))
    {
        Log(LOG_LEVEL_VERBOSE,
              "Allowing %s to connect without (re)checking ID\n", ip_assert);
        Log(LOG_LEVEL_VERBOSE,
              "Non-verified Host ID is %s (Using skipverify)\n", dns_assert);
        strncpy(conn->hostname, dns_assert, CF_MAXVARSIZE);
        Log(LOG_LEVEL_VERBOSE,
              "Non-verified User ID seems to be %s (Using skipverify)\n",
              username);
        strncpy(conn->username, username, CF_MAXVARSIZE);

#ifdef __MINGW32__            /* NT uses security identifier instead of uid */

        if (!NovaWin_UserNameToSid(username, (SID *) conn->sid,
                                   CF_MAXSIDSIZE, false))
        {
            memset(conn->sid, 0, CF_MAXSIDSIZE); /* is invalid sid - discarded */
        }

#else /* !__MINGW32__ */

        struct passwd *pw;
        if ((pw = getpwnam(username)) == NULL)    /* Keep this inside mutex */
        {
            conn->uid = -2;
        }
        else
        {
            conn->uid = pw->pw_uid;
        }

#endif /* !__MINGW32__ */
        return true;
    }

    if (strcmp(ip_assert, MapAddress(conn->ipaddr)) != 0)
    {
        Log(LOG_LEVEL_VERBOSE,
              "IP address mismatch between client's assertion (%s) "
              "and socket (%s) - untrustworthy connection\n",
              ip_assert, conn->ipaddr);
        return false;
    }

    if (strlen(dns_assert) == 0)
    {
        Log(LOG_LEVEL_VERBOSE,
              "DNS asserted name was empty - untrustworthy connection\n");
        return false;
    }

    if (strcmp(dns_assert, "skipident") == 0)
    {
        Log(LOG_LEVEL_VERBOSE,
              "DNS asserted name was withheld before key exchange"
              " - untrustworthy connection\n");
        return false;
    }

    Log(LOG_LEVEL_VERBOSE,
          "Socket caller address appears honest (%s matches %s)\n",
          ip_assert, MapAddress(conn->ipaddr));

    Log(LOG_LEVEL_VERBOSE, "Socket originates from %s=%s",
          ip_assert, dns_assert);

    Log(LOG_LEVEL_DEBUG, "Attempting to verify honesty by looking up hostname '%s'",
            dns_assert);

/* Do a reverse DNS lookup, like tcp wrappers to see if hostname matches IP */
    struct addrinfo *response, *ap;
    struct addrinfo query = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM
    };
    int err;

    err = getaddrinfo(dns_assert, NULL, &query, &response);
    if (err != 0)
    {
        Log(LOG_LEVEL_ERR,
              "VerifyConnection: Unable to lookup (%s): %s",
              dns_assert, gai_strerror(err));
    }
    else
    {
        for (ap = response; ap != NULL; ap = ap->ai_next)
        {
            /* No lookup, just convert ai_addr to string. */
            char txtaddr[CF_MAX_IP_LEN] = "";
            getnameinfo(ap->ai_addr, ap->ai_addrlen,
                        txtaddr, sizeof(txtaddr),
                        NULL, 0, NI_NUMERICHOST);

            if (strcmp(MapAddress(conn->ipaddr), txtaddr) == 0)
            {
                Log(LOG_LEVEL_DEBUG, "Found match");
                matched = true;
            }
        }
        freeaddrinfo(response);
    }


# ifdef __MINGW32__                   /* NT uses security identifier instead of uid */
    if (!NovaWin_UserNameToSid(username, (SID *) conn->sid, CF_MAXSIDSIZE, false))
    {
        memset(conn->sid, 0, CF_MAXSIDSIZE);    /* is invalid sid - discarded */
    }

# else/* !__MINGW32__ */
    struct passwd *pw;
    if ((pw = getpwnam(username)) == NULL)      /* Keep this inside mutex */
    {
        conn->uid = -2;
    }
    else
    {
        conn->uid = pw->pw_uid;
    }
# endif/* !__MINGW32__ */

    if (!matched)
    {
        Log(LOG_LEVEL_INFO, "Failed on DNS reverse lookup of '%s'. (gethostbyname: %s)",
            dns_assert, GetErrorStr());
        Log(LOG_LEVEL_INFO, "Client sent: %s", buf);
        return false;
    }

    Log(LOG_LEVEL_VERBOSE, "Host ID is %s", dns_assert);
    strncpy(conn->hostname, dns_assert, CF_MAXVARSIZE - 1);

    Log(LOG_LEVEL_VERBOSE, "User ID seems to be %s", username);
    strncpy(conn->username, username, CF_MAXVARSIZE - 1);

    return true;
}

/**************************************************************/

static int AllowedUser(char *user)
{
    if (IsItemIn(SV.allowuserlist, user))
    {
        Log(LOG_LEVEL_VERBOSE, "User %s granted connection privileges", user);
        return true;
    }

    Log(LOG_LEVEL_VERBOSE, "User %s is not allowed on this server", user);
    return false;
}

/**************************************************************/

/* 'resolved' argument needs to be at least CF_BUFSIZE long */

bool ResolveFilename(const char *req_path, char *res_path)
{
    char req_dir[CF_BUFSIZE];
    char req_filename[CF_BUFSIZE];

/*
 * Eliminate symlinks from path, but do not resolve the file itself if it is a
 * symlink.
 */

    strlcpy(req_dir, req_path, CF_BUFSIZE);
    ChopLastNode(req_dir);

    strlcpy(req_filename, ReadLastNode(req_path), CF_BUFSIZE);

#if defined HAVE_REALPATH && !defined _WIN32
    if (realpath(req_dir, res_path) == NULL)
    {
        return false;
    }
#else
    memset(res_path, 0, CF_BUFSIZE);
    CompressPath(res_path, req_dir);
#endif

    AddSlash(res_path);
    strlcat(res_path, req_filename, CF_BUFSIZE);

/* Adjust for forward slashes */

    MapName(res_path);

/* NT has case-insensitive path names */

#ifdef __MINGW32__
    int i;

    for (i = 0; i < strlen(res_path); i++)
    {
        res_path[i] = ToLower(res_path[i]);
    }
#endif /* __MINGW32__ */

    return true;
}

/**************************************************************/

static int AccessControl(EvalContext *ctx, const char *req_path, ServerConnectionState *conn, int encrypt)
{
    Auth *ap;
    int access = false;
    char transrequest[CF_BUFSIZE];
    struct stat statbuf;
    char translated_req_path[CF_BUFSIZE];
    char transpath[CF_BUFSIZE];

/*
 * /var/cfengine -> $workdir translation.
 */
    TranslatePath(translated_req_path, req_path);

    if (ResolveFilename(translated_req_path, transrequest))
    {
        Log(LOG_LEVEL_VERBOSE, "Filename %s is resolved to %s", translated_req_path, transrequest);
    }
    else
    {
        Log(LOG_LEVEL_VERBOSE, "Couldn't resolve filename '%s' from host '%s'. (lstat: %s)",
            translated_req_path, conn->hostname, GetErrorStr());
    }

    if (lstat(transrequest, &statbuf) == -1)
    {
        Log(LOG_LEVEL_VERBOSE, "Couldn't stat filename '%s' requested by host '%s'. (lstat: %s)",
            transrequest, conn->hostname, GetErrorStr());
        return false;
    }

    Log(LOG_LEVEL_DEBUG, "AccessControl, match (%s,%s) encrypt request = %d", transrequest, conn->hostname, encrypt);

    if (SV.admit == NULL)
    {
        Log(LOG_LEVEL_VERBOSE, "cf-serverd access list is empty, no files are visible");
        return false;
    }

    conn->maproot = false;

    for (ap = SV.admit; ap != NULL; ap = ap->next)
    {
        int res = false;

        Log(LOG_LEVEL_DEBUG, "Examining rule in access list (%s,%s)", transrequest, ap->path);

        strncpy(transpath, ap->path, CF_BUFSIZE - 1);
        MapName(transpath);

        if ((strlen(transrequest) > strlen(transpath)) && (strncmp(transpath, transrequest, strlen(transpath)) == 0)
            && (transrequest[strlen(transpath)] == FILE_SEPARATOR))
        {
            res = true;         /* Substring means must be a / to link, else just a substring og filename */
        }

        /* Exact match means single file to admit */

        if (strcmp(transpath, transrequest) == 0)
        {
            res = true;
        }

        if (strcmp(transpath, "/") == 0)
        {
            res = true;
        }

        if (res)
        {
            Log(LOG_LEVEL_VERBOSE, "Found a matching rule in access list (%s in %s)", transrequest, transpath);

            if (stat(transpath, &statbuf) == -1)
            {
                Log(LOG_LEVEL_INFO,
                      "Warning cannot stat file object %s in admit/grant, or access list refers to dangling link\n",
                      transpath);
                continue;
            }

            if ((!encrypt) && (ap->encrypt == true))
            {
                Log(LOG_LEVEL_ERR, "File %s requires encrypt connection...will not serve", transpath);
                access = false;
            }
            else
            {
                Log(LOG_LEVEL_DEBUG, "Checking whether to map root privileges..");

                if ((IsMatchItemIn(ctx, ap->maproot, MapAddress(conn->ipaddr))) || (IsRegexItemIn(ctx, ap->maproot, conn->hostname)))
                {
                    conn->maproot = true;
                    Log(LOG_LEVEL_VERBOSE, "Mapping root privileges to access non-root files");
                }

                if ((IsMatchItemIn(ctx, ap->accesslist, MapAddress(conn->ipaddr)))
                    || (IsRegexItemIn(ctx, ap->accesslist, conn->hostname)))
                {
                    access = true;
                    Log(LOG_LEVEL_DEBUG, "Access privileges - match found");
                }
            }
            break;
        }
    }

    if (strncmp(transpath, transrequest, strlen(transpath)) == 0)
    {
        for (ap = SV.deny; ap != NULL; ap = ap->next)
        {
            if (IsRegexItemIn(ctx, ap->accesslist, conn->hostname))
            {
                access = false;
                Log(LOG_LEVEL_VERBOSE, "Host %s explicitly denied access to %s", conn->hostname, transrequest);
                break;
            }
        }
    }

    if (access)
    {
        Log(LOG_LEVEL_VERBOSE, "Host %s granted access to %s", conn->hostname, req_path);

        if (encrypt && LOGENCRYPT)
        {
            /* Log files that were marked as requiring encryption */
            Log(LOG_LEVEL_INFO, "Host %s granted access to %s", conn->hostname, req_path);
        }
    }
    else
    {
        Log(LOG_LEVEL_VERBOSE, "Host %s denied access to %s", conn->hostname, req_path);
    }

    if (!conn->rsa_auth)
    {
        Log(LOG_LEVEL_VERBOSE, "Cannot map root access without RSA authentication");
        conn->maproot = false;  /* only public files accessible */
    }

    return access;
}

/**************************************************************/

static int LiteralAccessControl(EvalContext *ctx, char *in, ServerConnectionState *conn, int encrypt)
{
    Auth *ap;
    int access = false;
    char name[CF_BUFSIZE];

    name[0] = '\0';

    if (strncmp(in, "VAR", 3) == 0)
    {
        sscanf(in, "VAR %255[^\n]", name);
    }
    else if (strncmp(in, "CALL_ME_BACK", strlen("CALL_ME_BACK")) == 0)
    {
        sscanf(in, "CALL_ME_BACK %255[^\n]", name);
    }
    else
    {
        sscanf(in, "QUERY %128s", name);
    }

    conn->maproot = false;

    for (ap = SV.varadmit; ap != NULL; ap = ap->next)
    {
        int res = false;

        Log(LOG_LEVEL_VERBOSE, "Examining rule in access list (%s,%s)?", name, ap->path);

        if (strcmp(ap->path, name) == 0)
        {
            res = true;         /* Exact match means single file to admit */
        }

        if (res)
        {
            Log(LOG_LEVEL_VERBOSE, "Found a matching rule in access list (%s in %s)", name, ap->path);

            if ((!ap->literal) && (!ap->variable))
            {
                Log(LOG_LEVEL_ERR,
                    "Variable/query '%s' requires a literal server item...cannot set variable directly by path",
                      ap->path);
                access = false;
                break;
            }

            if ((!encrypt) && (ap->encrypt == true))
            {
                Log(LOG_LEVEL_ERR, "Variable %s requires encrypt connection...will not serve", name);
                access = false;
                break;
            }
            else
            {
                Log(LOG_LEVEL_DEBUG, "Checking whether to map root privileges");

                if ((IsMatchItemIn(ctx, ap->maproot, MapAddress(conn->ipaddr))) || (IsRegexItemIn(ctx, ap->maproot, conn->hostname)))
                {
                    conn->maproot = true;
                    Log(LOG_LEVEL_VERBOSE, "Mapping root privileges");
                }
                else
                {
                    Log(LOG_LEVEL_VERBOSE, "No root privileges granted");
                }

                if ((IsMatchItemIn(ctx, ap->accesslist, MapAddress(conn->ipaddr)))
                    || (IsRegexItemIn(ctx, ap->accesslist, conn->hostname)))
                {
                    access = true;
                    Log(LOG_LEVEL_DEBUG, "Access privileges - match found\n");
                }
            }
        }
    }

    for (ap = SV.vardeny; ap != NULL; ap = ap->next)
    {
        if (strcmp(ap->path, name) == 0)
        {
            if ((IsMatchItemIn(ctx, ap->accesslist, MapAddress(conn->ipaddr)))
                || (IsRegexItemIn(ctx, ap->accesslist, conn->hostname)))
            {
                access = false;
                Log(LOG_LEVEL_VERBOSE, "Host %s explicitly denied access to %s", conn->hostname, name);
                break;
            }
        }
    }

    if (access)
    {
        Log(LOG_LEVEL_VERBOSE, "Host %s granted access to literal '%s'", conn->hostname, name);

        if (encrypt && LOGENCRYPT)
        {
            /* Log files that were marked as requiring encryption */
            Log(LOG_LEVEL_INFO, "Host %s granted access to literal '%s'", conn->hostname, name);
        }
    }
    else
    {
        Log(LOG_LEVEL_VERBOSE, "Host %s denied access to literal '%s'", conn->hostname, name);
    }

    if (!conn->rsa_auth)
    {
        Log(LOG_LEVEL_VERBOSE, "Cannot map root access without RSA authentication");
        conn->maproot = false;  /* only public files accessible */
    }

    return access;
}

/**************************************************************/

static Item *ContextAccessControl(EvalContext *ctx, char *in, ServerConnectionState *conn, int encrypt)
{
    Auth *ap;
    int access = false;
    char client_regex[CF_BUFSIZE];
    CF_DB *dbp;
    CF_DBC *dbcp;
    int ksize, vsize;
    char *key;
    void *value;
    time_t now = time(NULL);
    CfState q;
    Item *ip, *matches = NULL, *candidates = NULL;

    sscanf(in, "CONTEXT %255[^\n]", client_regex);

    if (!OpenDB(&dbp, dbid_state))
    {
        return NULL;
    }

    if (!NewDBCursor(dbp, &dbcp))
    {
        Log(LOG_LEVEL_INFO, "Unable to scan persistence cache");
        CloseDB(dbp);
        return NULL;
    }

    while (NextDB(dbcp, &key, &ksize, &value, &vsize))
    {
        memcpy((void *) &q, value, sizeof(CfState));

        if (now > q.expires)
        {
            Log(LOG_LEVEL_VERBOSE, " Persistent class %s expired", key);
            DBCursorDeleteEntry(dbcp);
        }
        else
        {
            if (FullTextMatch(ctx, client_regex, key))
            {
                Log(LOG_LEVEL_VERBOSE, " - Found key %s...", key);
                AppendItem(&candidates, key, NULL);
            }
        }
    }

    DeleteDBCursor(dbcp);
    CloseDB(dbp);

    for (ip = candidates; ip != NULL; ip = ip->next)
    {
        for (ap = SV.varadmit; ap != NULL; ap = ap->next)
        {
            int res = false;

            if (FullTextMatch(ctx, ap->path, ip->name))
            {
                res = true;
            }

            if (res)
            {
                Log(LOG_LEVEL_VERBOSE, "Found a matching rule in access list (%s in %s)", ip->name, ap->path);

                if (ap->classpattern == false)
                {
                    Log(LOG_LEVEL_ERR,
                          "Context %s requires a literal server item...cannot set variable directly by path",
                          ap->path);
                    access = false;
                    continue;
                }

                if ((!encrypt) && (ap->encrypt == true))
                {
                    Log(LOG_LEVEL_ERR, "Context %s requires encrypt connection...will not serve", ip->name);
                    access = false;
                    break;
                }
                else
                {
                    Log(LOG_LEVEL_DEBUG, "Checking whether to map root privileges");

                    if ((IsMatchItemIn(ctx, ap->maproot, MapAddress(conn->ipaddr)))
                        || (IsRegexItemIn(ctx, ap->maproot, conn->hostname)))
                    {
                        conn->maproot = true;
                        Log(LOG_LEVEL_VERBOSE, "Mapping root privileges");
                    }
                    else
                    {
                        Log(LOG_LEVEL_VERBOSE, "No root privileges granted");
                    }

                    if ((IsMatchItemIn(ctx, ap->accesslist, MapAddress(conn->ipaddr)))
                        || (IsRegexItemIn(ctx, ap->accesslist, conn->hostname)))
                    {
                        access = true;
                        Log(LOG_LEVEL_DEBUG, "Access privileges - match found");
                    }
                }
            }
        }

        for (ap = SV.vardeny; ap != NULL; ap = ap->next)
        {
            if (strcmp(ap->path, ip->name) == 0)
            {
                if ((IsMatchItemIn(ctx, ap->accesslist, MapAddress(conn->ipaddr)))
                    || (IsRegexItemIn(ctx, ap->accesslist, conn->hostname)))
                {
                    access = false;
                    Log(LOG_LEVEL_VERBOSE, "Host %s explicitly denied access to context %s", conn->hostname, ip->name);
                    break;
                }
            }
        }

        if (access)
        {
            Log(LOG_LEVEL_VERBOSE, "Host %s granted access to context '%s'", conn->hostname, ip->name);
            AppendItem(&matches, ip->name, NULL);

            if (encrypt && LOGENCRYPT)
            {
                /* Log files that were marked as requiring encryption */
                Log(LOG_LEVEL_INFO, "Host %s granted access to context '%s'", conn->hostname, ip->name);
            }
        }
        else
        {
            Log(LOG_LEVEL_VERBOSE, "Host %s denied access to context '%s'", conn->hostname, ip->name);
        }
    }

    DeleteItemList(candidates);
    return matches;
}

/**************************************************************/

static int AuthorizeRoles(EvalContext *ctx, ServerConnectionState *conn, char *args)
{
    char *sp;
    Auth *ap;
    char userid1[CF_MAXVARSIZE], userid2[CF_MAXVARSIZE];
    Rlist *rp, *defines = NULL;
    int permitted = false;

    snprintf(userid1, CF_MAXVARSIZE, "%s@%s", conn->username, conn->hostname);
    snprintf(userid2, CF_MAXVARSIZE, "%s@%s", conn->username, conn->ipaddr);

    Log(LOG_LEVEL_VERBOSE, "Checking authorized roles in %s", args);

    if (strncmp(args, "--define", strlen("--define")) == 0)
    {
        sp = args + strlen("--define");
    }
    else
    {
        sp = args + strlen("-D");
    }

    while (*sp == ' ')
    {
        sp++;
    }

    defines = RlistFromSplitRegex(ctx, sp, "[,:;]", 99, false);

/* For each user-defined class attempt, check RBAC */

    for (rp = defines; rp != NULL; rp = rp->next)
    {
        Log(LOG_LEVEL_VERBOSE, "Verifying %s", RlistScalarValue(rp));

        for (ap = SV.roles; ap != NULL; ap = ap->next)
        {
            if (FullTextMatch(ctx, ap->path, rp->item))
            {
                /* We have a pattern covering this class - so are we allowed to activate it? */
                if ((IsMatchItemIn(ctx, ap->accesslist, MapAddress(conn->ipaddr))) ||
                    (IsRegexItemIn(ctx, ap->accesslist, conn->hostname)) ||
                    (IsRegexItemIn(ctx, ap->accesslist, userid1)) ||
                    (IsRegexItemIn(ctx, ap->accesslist, userid2)) ||
                    (IsRegexItemIn(ctx, ap->accesslist, conn->username)))
                {
                    Log(LOG_LEVEL_VERBOSE, "Attempt to define role/class %s is permitted", RlistScalarValue(rp));
                    permitted = true;
                }
                else
                {
                    Log(LOG_LEVEL_VERBOSE, "Attempt to define role/class %s is denied", RlistScalarValue(rp));
                    RlistDestroy(defines);
                    return false;
                }
            }
        }

    }

    if (permitted)
    {
        Log(LOG_LEVEL_VERBOSE, "Role activation allowed");
    }
    else
    {
        Log(LOG_LEVEL_VERBOSE, "Role activation disallowed - abort execution");
    }

    RlistDestroy(defines);
    return permitted;
}

/**************************************************************/

static int AuthenticationDialogue(ServerConnectionState *conn, char *recvbuffer, int recvlen)
{
    char in[CF_BUFSIZE], *out, *decrypted_nonce;
    BIGNUM *counter_challenge = NULL;
    unsigned char digest[EVP_MAX_MD_SIZE + 1] = { 0 };
    unsigned int crypt_len, nonce_len = 0, encrypted_len = 0;
    char sauth[10], iscrypt = 'n', enterprise_field = 'c';
    int len_n = 0, len_e = 0, keylen, session_size;
    unsigned long err;
    RSA *newkey;
    int digestLen = 0;
    HashMethod digestType;

    if ((PRIVKEY == NULL) || (PUBKEY == NULL))
    {
        Log(LOG_LEVEL_ERR, "No public/private key pair exists, create one with cf-key");
        return false;
    }

    if (FIPS_MODE)
    {
        digestType = CF_DEFAULT_DIGEST;
        digestLen = CF_DEFAULT_DIGEST_LEN;
    }
    else
    {
        digestType = HASH_METHOD_MD5;
        digestLen = CF_MD5_LEN;
    }

/* proposition C1 */
/* Opening string is a challenge from the client (some agent) */

    sauth[0] = '\0';

    sscanf(recvbuffer, "%s %c %u %u %c", sauth, &iscrypt, &crypt_len, &nonce_len, &enterprise_field);

    if ((crypt_len == 0) || (nonce_len == 0) || (strlen(sauth) == 0))
    {
        Log(LOG_LEVEL_INFO, "Protocol format error in authentation from IP %s", conn->hostname);
        return false;
    }

    if (nonce_len > CF_NONCELEN * 2)
    {
        Log(LOG_LEVEL_INFO, "Protocol deviant authentication nonce from %s", conn->hostname);
        return false;
    }

    if (crypt_len > 2 * CF_NONCELEN)
    {
        Log(LOG_LEVEL_INFO, "Protocol abuse in unlikely cipher from %s", conn->hostname);
        return false;
    }

/* Check there is no attempt to read past the end of the received input */

    if (recvbuffer + CF_RSA_PROTO_OFFSET + nonce_len > recvbuffer + recvlen)
    {
        Log(LOG_LEVEL_INFO, "Protocol consistency error in authentication from %s", conn->hostname);
        return false;
    }

    if ((strcmp(sauth, "SAUTH") != 0) || (nonce_len == 0) || (crypt_len == 0))
    {
        Log(LOG_LEVEL_INFO, "Protocol error in RSA authentication from IP '%s'", conn->hostname);
        return false;
    }

    Log(LOG_LEVEL_DEBUG, "Challenge encryption = %c, nonce = %d, buf = %d", iscrypt, nonce_len, crypt_len);


    decrypted_nonce = xmalloc(crypt_len);

    if (iscrypt == 'y')
    {
        if (RSA_private_decrypt
            (crypt_len, recvbuffer + CF_RSA_PROTO_OFFSET, decrypted_nonce, PRIVKEY, RSA_PKCS1_PADDING) <= 0)
        {
            err = ERR_get_error();

            Log(LOG_LEVEL_ERR, "Private decrypt failed = '%s'", ERR_reason_error_string(err));
            free(decrypted_nonce);
            return false;
        }
    }
    else
    {
        if (nonce_len > crypt_len)
        {
            Log(LOG_LEVEL_ERR, "Illegal challenge");
            free(decrypted_nonce);
            return false;
        }

        memcpy(decrypted_nonce, recvbuffer + CF_RSA_PROTO_OFFSET, nonce_len);
    }

/* Client's ID is now established by key or trusted, reply with digest */

    HashString(decrypted_nonce, nonce_len, digest, digestType);

    free(decrypted_nonce);

/* Get the public key from the client */
    newkey = RSA_new();

/* proposition C2 */
    if ((len_n = ReceiveTransaction(&conn->conn_info, recvbuffer, NULL)) == -1)
    {
        Log(LOG_LEVEL_INFO, "Protocol error 1 in RSA authentation from IP %s", conn->hostname);
        RSA_free(newkey);
        return false;
    }

    if (len_n == 0)
    {
        Log(LOG_LEVEL_INFO, "Protocol error 2 in RSA authentation from IP %s", conn->hostname);
        RSA_free(newkey);
        return false;
    }

    if ((newkey->n = BN_mpi2bn(recvbuffer, len_n, NULL)) == NULL)
    {
        err = ERR_get_error();
        Log(LOG_LEVEL_ERR, "Private decrypt failed = %s", ERR_reason_error_string(err));
        RSA_free(newkey);
        return false;
    }

/* proposition C3 */

    if ((len_e = ReceiveTransaction(&conn->conn_info, recvbuffer, NULL)) == -1)
    {
        Log(LOG_LEVEL_INFO, "Protocol error 3 in RSA authentation from IP %s", conn->hostname);
        RSA_free(newkey);
        return false;
    }

    if (len_e == 0)
    {
        Log(LOG_LEVEL_INFO, "Protocol error 4 in RSA authentation from IP %s", conn->hostname);
        RSA_free(newkey);
        return false;
    }

    if ((newkey->e = BN_mpi2bn(recvbuffer, len_e, NULL)) == NULL)
    {
        err = ERR_get_error();
        Log(LOG_LEVEL_ERR, "Private decrypt failed = %s", ERR_reason_error_string(err));
        RSA_free(newkey);
        return false;
    }

    HashPubKey(newkey, conn->digest, CF_DEFAULT_DIGEST);

    {
        char buffer[EVP_MAX_MD_SIZE * 4];
        Log(LOG_LEVEL_VERBOSE, "Public key identity of host '%s' is '%s'", conn->ipaddr,
              HashPrintSafe(CF_DEFAULT_DIGEST, conn->digest, buffer));
    }

    LastSaw(conn->ipaddr, conn->digest, LAST_SEEN_ROLE_ACCEPT);

    if (!CheckStoreKey(conn, newkey))   /* conceals proposition S1 */
    {
        if (!conn->trust)
        {
            RSA_free(newkey);
            return false;
        }
    }

/* Reply with digest of original challenge */

/* proposition S2 */

    SendTransaction(&conn->conn_info, digest, digestLen, CF_DONE);

/* Send counter challenge to be sure this is a live session */

    counter_challenge = BN_new();
    if (counter_challenge == NULL)
    {
        Log(LOG_LEVEL_ERR, "Cannot allocate BIGNUM structure for counter challenge");
        RSA_free(newkey);
        return false;
    }

    BN_rand(counter_challenge, CF_NONCELEN, 0, 0);
    nonce_len = BN_bn2mpi(counter_challenge, in);

// hash the challenge from the client

    HashString(in, nonce_len, digest, digestType);

    encrypted_len = RSA_size(newkey);   /* encryption buffer is always the same size as n */

    out = xmalloc(encrypted_len + 1);

    if (RSA_public_encrypt(nonce_len, in, out, newkey, RSA_PKCS1_PADDING) <= 0)
    {
        err = ERR_get_error();
        Log(LOG_LEVEL_ERR, "Public encryption failed = %s", ERR_reason_error_string(err));
        RSA_free(newkey);
        free(out);
        return false;
    }

/* proposition S3 */
    SendTransaction(&conn->conn_info, out, encrypted_len, CF_DONE);

/* if the client doesn't have our public key, send it */

    if (iscrypt != 'y')
    {
        /* proposition S4  - conditional */
        memset(in, 0, CF_BUFSIZE);
        len_n = BN_bn2mpi(PUBKEY->n, in);
        SendTransaction(&conn->conn_info, in, len_n, CF_DONE);

        /* proposition S5  - conditional */
        memset(in, 0, CF_BUFSIZE);
        len_e = BN_bn2mpi(PUBKEY->e, in);
        SendTransaction(&conn->conn_info, in, len_e, CF_DONE);
    }

/* Receive reply to counter_challenge */

/* proposition C4 */
    memset(in, 0, CF_BUFSIZE);

    if (ReceiveTransaction(&conn->conn_info, in, NULL) == -1)
    {
        BN_free(counter_challenge);
        free(out);
        RSA_free(newkey);
        return false;
    }

    if (HashesMatch(digest, in, digestType))    /* replay / piggy in the middle attack ? */
    {
        if (!conn->trust)
        {
            Log(LOG_LEVEL_VERBOSE, "Strong authentication of client %s/%s achieved", conn->hostname, conn->ipaddr);
        }
        else
        {
            Log(LOG_LEVEL_VERBOSE, "Weak authentication of trusted client %s/%s (key accepted on trust).",
                  conn->hostname, conn->ipaddr);
        }
    }
    else
    {
        BN_free(counter_challenge);
        free(out);
        RSA_free(newkey);
        Log(LOG_LEVEL_INFO, "Challenge response from client %s was incorrect - ID false?", conn->ipaddr);
        return false;
    }

/* Receive random session key,... */

/* proposition C5 */

    memset(in, 0, CF_BUFSIZE);

    if ((keylen = ReceiveTransaction(&conn->conn_info, in, NULL)) == -1)
    {
        BN_free(counter_challenge);
        free(out);
        RSA_free(newkey);
        return false;
    }

    if (keylen > CF_BUFSIZE / 2)
    {
        BN_free(counter_challenge);
        free(out);
        RSA_free(newkey);
        Log(LOG_LEVEL_INFO, "Session key length received from %s is too long", conn->ipaddr);
        return false;
    }

    session_size = CfSessionKeySize(enterprise_field);
    conn->session_key = xmalloc(session_size);
    conn->encryption_type = enterprise_field;

    Log(LOG_LEVEL_VERBOSE, "Receiving session key from client (size=%d)...", keylen);

    Log(LOG_LEVEL_DEBUG, "keylen = %d, session_size = %d", keylen, session_size);

    if (keylen == CF_BLOWFISHSIZE)      /* Support the old non-ecnrypted for upgrade */
    {
        memcpy(conn->session_key, in, session_size);
    }
    else
    {
        /* New protocol encrypted */

        if (RSA_private_decrypt(keylen, in, out, PRIVKEY, RSA_PKCS1_PADDING) <= 0)
        {
            err = ERR_get_error();
            Log(LOG_LEVEL_ERR, "Private decrypt failed = %s", ERR_reason_error_string(err));
            return false;
        }

        memcpy(conn->session_key, out, session_size);
    }

    BN_free(counter_challenge);
    free(out);
    RSA_free(newkey);
    conn->rsa_auth = true;
    return true;
}

/**************************************************************/

static int StatFile(ServerConnectionState *conn, char *sendbuffer, char *ofilename)
/* Because we do not know the size or structure of remote datatypes,*/
/* the simplest way to transfer the data is to convert them into */
/* plain text and interpret them on the other side. */
{
    Stat cfst;
    struct stat statbuf, statlinkbuf;
    char linkbuf[CF_BUFSIZE], filename[CF_BUFSIZE];
    int islink = false;

    TranslatePath(filename, ofilename);

    memset(&cfst, 0, sizeof(Stat));

    if (strlen(ReadLastNode(filename)) > CF_MAXLINKSIZE)
    {
        snprintf(sendbuffer, CF_BUFSIZE, "BAD: Filename suspiciously long [%s]\n", filename);
        Log(LOG_LEVEL_ERR, "%s", sendbuffer);
        SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
        return -1;
    }

    if (lstat(filename, &statbuf) == -1)
    {
        snprintf(sendbuffer, CF_BUFSIZE, "BAD: unable to stat file %s", filename);
        Log(LOG_LEVEL_VERBOSE, "%s. (lstat: %s)", sendbuffer, GetErrorStr());
        SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
        return -1;
    }

    cfst.cf_readlink = NULL;
    cfst.cf_lmode = 0;
    cfst.cf_nlink = CF_NOSIZE;

    memset(linkbuf, 0, CF_BUFSIZE);

#ifndef __MINGW32__                   // windows doesn't support symbolic links
    if (S_ISLNK(statbuf.st_mode))
    {
        islink = true;
        cfst.cf_type = FILE_TYPE_LINK; /* pointless - overwritten */
        cfst.cf_lmode = statbuf.st_mode & 07777;
        cfst.cf_nlink = statbuf.st_nlink;

        if (readlink(filename, linkbuf, CF_BUFSIZE - 1) == -1)
        {
            sprintf(sendbuffer, "BAD: unable to read link\n");
            Log(LOG_LEVEL_ERR, "%s. (readlink: %s)", sendbuffer, GetErrorStr());
            SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
            return -1;
        }

        Log(LOG_LEVEL_DEBUG, "readlink '%s'", linkbuf);

        cfst.cf_readlink = linkbuf;
    }
#endif /* !__MINGW32__ */

    if ((!islink) && (stat(filename, &statbuf) == -1))
    {
        Log(LOG_LEVEL_VERBOSE, "BAD: unable to stat file '%s'. (stat: %s)",
            filename, GetErrorStr());
        SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
        return -1;
    }

    Log(LOG_LEVEL_DEBUG, "Getting size of link deref '%s'", linkbuf);

    if (islink && (stat(filename, &statlinkbuf) != -1))       /* linktype=copy used by agent */
    {
        statbuf.st_size = statlinkbuf.st_size;
        statbuf.st_mode = statlinkbuf.st_mode;
        statbuf.st_uid = statlinkbuf.st_uid;
        statbuf.st_gid = statlinkbuf.st_gid;
        statbuf.st_mtime = statlinkbuf.st_mtime;
        statbuf.st_ctime = statlinkbuf.st_ctime;
    }

    if (S_ISDIR(statbuf.st_mode))
    {
        cfst.cf_type = FILE_TYPE_DIR;
    }

    if (S_ISREG(statbuf.st_mode))
    {
        cfst.cf_type = FILE_TYPE_REGULAR;
    }

    if (S_ISSOCK(statbuf.st_mode))
    {
        cfst.cf_type = FILE_TYPE_SOCK;
    }

    if (S_ISCHR(statbuf.st_mode))
    {
        cfst.cf_type = FILE_TYPE_CHAR_;
    }

    if (S_ISBLK(statbuf.st_mode))
    {
        cfst.cf_type = FILE_TYPE_BLOCK;
    }

    if (S_ISFIFO(statbuf.st_mode))
    {
        cfst.cf_type = FILE_TYPE_FIFO;
    }

    cfst.cf_mode = statbuf.st_mode & 07777;
    cfst.cf_uid = statbuf.st_uid & 0xFFFFFFFF;
    cfst.cf_gid = statbuf.st_gid & 0xFFFFFFFF;
    cfst.cf_size = statbuf.st_size;
    cfst.cf_atime = statbuf.st_atime;
    cfst.cf_mtime = statbuf.st_mtime;
    cfst.cf_ctime = statbuf.st_ctime;
    cfst.cf_ino = statbuf.st_ino;
    cfst.cf_dev = statbuf.st_dev;
    cfst.cf_readlink = linkbuf;

    if (cfst.cf_nlink == CF_NOSIZE)
    {
        cfst.cf_nlink = statbuf.st_nlink;
    }

#if !defined(__MINGW32__)
    if (statbuf.st_size > statbuf.st_blocks * DEV_BSIZE)
#else
# ifdef HAVE_ST_BLOCKS
    if (statbuf.st_size > statbuf.st_blocks * DEV_BSIZE)
# else
    if (statbuf.st_size > ST_NBLOCKS(statbuf) * DEV_BSIZE)
# endif
#endif
    {
        cfst.cf_makeholes = 1;  /* must have a hole to get checksum right */
    }
    else
    {
        cfst.cf_makeholes = 0;
    }

    memset(sendbuffer, 0, CF_BUFSIZE);

    /* send as plain text */

    Log(LOG_LEVEL_DEBUG, "OK: type = %d, mode = %" PRIoMAX ", lmode = %" PRIoMAX ", uid = %" PRIuMAX ", gid = %" PRIuMAX ", size = %" PRIdMAX ", atime=%" PRIdMAX ", mtime = %" PRIdMAX,
            cfst.cf_type, (uintmax_t)cfst.cf_mode, (uintmax_t)cfst.cf_lmode, (intmax_t)cfst.cf_uid, (intmax_t)cfst.cf_gid, (intmax_t) cfst.cf_size,
            (intmax_t) cfst.cf_atime, (intmax_t) cfst.cf_mtime);

    snprintf(sendbuffer, CF_BUFSIZE, "OK: %d %ju %ju %ju %ju %jd %jd %jd %jd %d %d %d %jd",
             cfst.cf_type, (uintmax_t)cfst.cf_mode, (uintmax_t)cfst.cf_lmode,
             (uintmax_t)cfst.cf_uid, (uintmax_t)cfst.cf_gid, (intmax_t)cfst.cf_size,
             (intmax_t) cfst.cf_atime, (intmax_t) cfst.cf_mtime, (intmax_t) cfst.cf_ctime,
             cfst.cf_makeholes, cfst.cf_ino, cfst.cf_nlink, (intmax_t) cfst.cf_dev);

    SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);

    memset(sendbuffer, 0, CF_BUFSIZE);

    if (cfst.cf_readlink != NULL)
    {
        strcpy(sendbuffer, "OK:");
        strcat(sendbuffer, cfst.cf_readlink);
    }
    else
    {
        sprintf(sendbuffer, "OK:");
    }

    SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
    return 0;
}

/***************************************************************/

static void CfGetFile(ServerFileGetState *args)
{
    int fd;
    off_t n_read, total = 0, sendlen = 0, count = 0;
    char sendbuffer[CF_BUFSIZE + 256], filename[CF_BUFSIZE];
    struct stat sb;
    int blocksize = 2048;

    ConnectionInfo *conn_info = &(args->connect)->conn_info;

    TranslatePath(filename, args->replyfile);

    stat(filename, &sb);

    Log(LOG_LEVEL_DEBUG, "CfGetFile('%s'), size = %" PRIdMAX, filename, (intmax_t) sb.st_size);

/* Now check to see if we have remote permission */

    if (!TransferRights(filename, args, &sb))
    {
        RefuseAccess(args->connect, args->buf_size, "");
        snprintf(sendbuffer, CF_BUFSIZE, "%s", CF_FAILEDSTR);
        if (conn_info->type == CF_PROTOCOL_CLASSIC)
        {
            SendSocketStream(conn_info->sd, sendbuffer, args->buf_size);
        }
        else if (conn_info->type == CF_PROTOCOL_TLS)
        {
            TLSSend(conn_info->ssl, sendbuffer, args->buf_size);
        }
        return;
    }

/* File transfer */

    if ((fd = open(filename, O_RDONLY)) == -1)
    {
        Log(LOG_LEVEL_ERR, "Open error of file '%s'. (open: %s)",
            filename, GetErrorStr());
        snprintf(sendbuffer, CF_BUFSIZE, "%s", CF_FAILEDSTR);
        if (conn_info->type == CF_PROTOCOL_CLASSIC)
        {
            SendSocketStream(conn_info->sd, sendbuffer, args->buf_size);
        }
        else if (conn_info->type == CF_PROTOCOL_TLS)
        {
            TLSSend(conn_info->ssl, sendbuffer, args->buf_size);
        }
    }
    else
    {
        int div = 3;

        if (sb.st_size > 10485760L) /* File larger than 10 MB, checks every 64kB */
        {
            div = 32;
        }

        while (true)
        {
            memset(sendbuffer, 0, CF_BUFSIZE);

            Log(LOG_LEVEL_DEBUG, "Now reading from disk...");

            if ((n_read = read(fd, sendbuffer, blocksize)) == -1)
            {
                Log(LOG_LEVEL_ERR, "Read failed in GetFile. (read: %s)", GetErrorStr());
                break;
            }

            if (n_read == 0)
            {
                break;
            }
            else
            {
                off_t savedlen = sb.st_size;

                /* check the file is not changing at source */

                if (count++ % div == 0)   /* Don't do this too often */
                {
                    if (stat(filename, &sb))
                    {
                        Log(LOG_LEVEL_ERR, "Cannot stat file '%s'. (stat: %s)",
                            filename, GetErrorStr());
                        break;
                    }
                }

                if (sb.st_size != savedlen)
                {
                    snprintf(sendbuffer, CF_BUFSIZE, "%s%s: %s", CF_CHANGEDSTR1, CF_CHANGEDSTR2, filename);

                    if (conn_info->type == CF_PROTOCOL_CLASSIC)
                    {
                        if (SendSocketStream(conn_info->sd, sendbuffer, blocksize) == -1)
                        {
                            Log(LOG_LEVEL_VERBOSE, "Send failed in GetFile. (send: %s)", GetErrorStr());
                        }
                    }
                    else if (conn_info->type == CF_PROTOCOL_TLS)
                    {
                        if (TLSSend(conn_info->ssl, sendbuffer, blocksize) == -1)
                        {
                            Log(LOG_LEVEL_VERBOSE, "Send failed in GetFile. (send: %s)", GetErrorStr());
                        }
                    }

                    Log(LOG_LEVEL_DEBUG, "Aborting transfer after %" PRIdMAX ": file is changing rapidly at source.", (intmax_t)total);
                    break;
                }

                if ((savedlen - total) / blocksize > 0)
                {
                    sendlen = blocksize;
                }
                else if (savedlen != 0)
                {
                    sendlen = (savedlen - total);
                }
            }

            total += n_read;

            if (conn_info->type == CF_PROTOCOL_CLASSIC)
            {
                if (SendSocketStream(conn_info->sd, sendbuffer, sendlen) == -1)
                {
                    Log(LOG_LEVEL_VERBOSE, "Send failed in GetFile. (send: %s)", GetErrorStr());
                    break;
                }
            }
            else if (conn_info->type == CF_PROTOCOL_TLS)
            {
                if (TLSSend(conn_info->ssl, sendbuffer, sendlen) == -1)
                {
                    Log(LOG_LEVEL_VERBOSE, "Send failed in GetFile. (send: %s)", GetErrorStr());
                    break;
                }
            }
        }

        close(fd);
    }
}

/***************************************************************/

static void CfEncryptGetFile(ServerFileGetState *args)
/* Because the stream doesn't end for each file, we need to know the
   exact number of bytes transmitted, which might change during
   encryption, hence we need to handle this with transactions */
{
    int fd, n_read, cipherlen, finlen;
    off_t total = 0, count = 0;
    char sendbuffer[CF_BUFSIZE + 256], out[CF_BUFSIZE], filename[CF_BUFSIZE];
    unsigned char iv[32] =
        { 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8 };
    int blocksize = CF_BUFSIZE - 4 * CF_INBAND_OFFSET;
    EVP_CIPHER_CTX ctx;
    char *key, enctype;
    struct stat sb;
    ConnectionInfo *conn_info = &args->connect->conn_info;

    key = (args->connect)->session_key;
    enctype = (args->connect)->encryption_type;

    TranslatePath(filename, args->replyfile);

    stat(filename, &sb);

    Log(LOG_LEVEL_DEBUG, "CfEncryptGetFile('%s'), size = %" PRIdMAX, filename, (intmax_t) sb.st_size);

/* Now check to see if we have remote permission */

    if (!TransferRights(filename, args, &sb))
    {
        RefuseAccess(args->connect, args->buf_size, "");
        FailedTransfer(conn_info);
    }

    EVP_CIPHER_CTX_init(&ctx);

    if ((fd = open(filename, O_RDONLY)) == -1)
    {
        Log(LOG_LEVEL_ERR, "Open error of file '%s'. (open: %s)", filename, GetErrorStr());
        FailedTransfer(conn_info);
    }
    else
    {
        int div = 3;

        if (sb.st_size > 10485760L) /* File larger than 10 MB, checks every 64kB */
        {
            div = 32;
        }

        while (true)
        {
            memset(sendbuffer, 0, CF_BUFSIZE);

            if ((n_read = read(fd, sendbuffer, blocksize)) == -1)
            {
                Log(LOG_LEVEL_ERR, "Read failed in EncryptGetFile. (read: %s)", GetErrorStr());
                break;
            }

            off_t savedlen = sb.st_size;

            if (count++ % div == 0)       /* Don't do this too often */
            {
                Log(LOG_LEVEL_DEBUG, "Restatting '%s' - size %d", filename, n_read);
                if (stat(filename, &sb))
                {
                    Log(LOG_LEVEL_ERR, "Cannot stat file '%s' (stat: %s)",
                            filename, GetErrorStr());
                    break;
                }
            }

            if (sb.st_size != savedlen)
            {
                AbortTransfer(conn_info, filename);
                break;
            }

            total += n_read;

            if (n_read > 0)
            {
                EVP_EncryptInit_ex(&ctx, CfengineCipher(enctype), NULL, key, iv);

                if (!EVP_EncryptUpdate(&ctx, out, &cipherlen, sendbuffer, n_read))
                {
                    FailedTransfer(conn_info);
                    EVP_CIPHER_CTX_cleanup(&ctx);
                    close(fd);
                    return;
                }

                if (!EVP_EncryptFinal_ex(&ctx, out + cipherlen, &finlen))
                {
                    FailedTransfer(conn_info);
                    EVP_CIPHER_CTX_cleanup(&ctx);
                    close(fd);
                    return;
                }
            }

            if (total >= savedlen)
            {
                if (SendTransaction(conn_info, out, cipherlen + finlen, CF_DONE) == -1)
                {
                    Log(LOG_LEVEL_VERBOSE, "Send failed in GetFile. (send: %s)", GetErrorStr());
                    EVP_CIPHER_CTX_cleanup(&ctx);
                    close(fd);
                    return;
                }
                break;
            }
            else
            {
                if (SendTransaction(conn_info, out, cipherlen + finlen, CF_MORE) == -1)
                {
                    Log(LOG_LEVEL_VERBOSE, "Send failed in GetFile. (send: %s)", GetErrorStr());
                    close(fd);
                    EVP_CIPHER_CTX_cleanup(&ctx);
                    return;
                }
            }
        }
    }

    EVP_CIPHER_CTX_cleanup(&ctx);
    close(fd);
}

/**************************************************************/

static void CompareLocalHash(ServerConnectionState *conn, char *sendbuffer, char *recvbuffer)
{
    unsigned char digest1[EVP_MAX_MD_SIZE + 1], digest2[EVP_MAX_MD_SIZE + 1];
    char filename[CF_BUFSIZE], rfilename[CF_BUFSIZE];
    char *sp;
    int i;

/* TODO - when safe change this proto string to sha2 */

    sscanf(recvbuffer, "MD5 %[^\n]", rfilename);

    sp = recvbuffer + strlen(recvbuffer) + CF_SMALL_OFFSET;

    for (i = 0; i < CF_DEFAULT_DIGEST_LEN; i++)
    {
        digest1[i] = *sp++;
    }

    memset(sendbuffer, 0, CF_BUFSIZE);

    TranslatePath(filename, rfilename);

    HashFile(filename, digest2, CF_DEFAULT_DIGEST);

    if ((HashesMatch(digest1, digest2, CF_DEFAULT_DIGEST)) || (HashesMatch(digest1, digest2, HASH_METHOD_MD5)))
    {
        sprintf(sendbuffer, "%s", CFD_FALSE);
        Log(LOG_LEVEL_DEBUG, "Hashes matched ok");
        SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
    }
    else
    {
        sprintf(sendbuffer, "%s", CFD_TRUE);
        Log(LOG_LEVEL_DEBUG, "Hashes didn't match");
        SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
    }
}

/**************************************************************/

static void GetServerLiteral(EvalContext *ctx, ServerConnectionState *conn, char *sendbuffer, char *recvbuffer, int encrypted)
{
    char handle[CF_BUFSIZE], out[CF_BUFSIZE];
    int cipherlen;

    sscanf(recvbuffer, "VAR %255[^\n]", handle);

    if (ReturnLiteralData(ctx, handle, out))
    {
        memset(sendbuffer, 0, CF_BUFSIZE);
        snprintf(sendbuffer, CF_BUFSIZE - 1, "%s", out);
    }
    else
    {
        memset(sendbuffer, 0, CF_BUFSIZE);
        snprintf(sendbuffer, CF_BUFSIZE - 1, "BAD: Not found");
    }

    if (encrypted)
    {
        cipherlen = EncryptString(conn->encryption_type, sendbuffer, out, conn->session_key, strlen(sendbuffer) + 1);
        SendTransaction(&conn->conn_info, out, cipherlen, CF_DONE);
    }
    else
    {
        SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
    }
}

/********************************************************************/

static int GetServerQuery(ServerConnectionState *conn, char *recvbuffer)
{
    char query[CF_BUFSIZE];

    query[0] = '\0';
    sscanf(recvbuffer, "QUERY %255[^\n]", query);

    if (strlen(query) == 0)
    {
        return false;
    }

    return ReturnQueryData(conn, query);
}

/**************************************************************/

static void ReplyServerContext(ServerConnectionState *conn, int encrypted, Item *classes)
{
    char out[CF_BUFSIZE];
    int cipherlen;
    Item *ip;

    char sendbuffer[CF_BUFSIZE];
    memset(sendbuffer, 0, CF_BUFSIZE);

    for (ip = classes; ip != NULL; ip = ip->next)
    {
        if (strlen(sendbuffer) + strlen(ip->name) < CF_BUFSIZE - 3)
        {
            strcat(sendbuffer, ip->name);
            strcat(sendbuffer, ",");
        }
        else
        {
            Log(LOG_LEVEL_ERR, "Overflow in context grab");
            break;
        }
    }

    DeleteItemList(classes);

    if (encrypted)
    {
        cipherlen = EncryptString(conn->encryption_type, sendbuffer, out, conn->session_key, strlen(sendbuffer) + 1);
        SendTransaction(&conn->conn_info, out, cipherlen, CF_DONE);
    }
    else
    {
        SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
    }
}

/**************************************************************/

static int CfOpenDirectory(ServerConnectionState *conn, char *sendbuffer, char *oldDirname)
{
    Dir *dirh;
    const struct dirent *dirp;
    int offset;
    char dirname[CF_BUFSIZE];

    TranslatePath(dirname, oldDirname);

    if (!IsAbsoluteFileName(dirname))
    {
        sprintf(sendbuffer, "BAD: request to access a non-absolute filename\n");
        SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
        return -1;
    }

    if ((dirh = DirOpen(dirname)) == NULL)
    {
        Log(LOG_LEVEL_DEBUG, "Couldn't open dir '%s'", dirname);
        snprintf(sendbuffer, CF_BUFSIZE, "BAD: cfengine, couldn't open dir %s\n", dirname);
        SendTransaction(&conn->conn_info, sendbuffer, 0, CF_DONE);
        return -1;
    }

/* Pack names for transmission */

    memset(sendbuffer, 0, CF_BUFSIZE);

    offset = 0;

    for (dirp = DirRead(dirh); dirp != NULL; dirp = DirRead(dirh))
    {
        if (strlen(dirp->d_name) + 1 + offset >= CF_BUFSIZE - CF_MAXLINKSIZE)
        {
            SendTransaction(&conn->conn_info, sendbuffer, offset + 1, CF_MORE);
            offset = 0;
            memset(sendbuffer, 0, CF_BUFSIZE);
        }

        strncpy(sendbuffer + offset, dirp->d_name, CF_MAXLINKSIZE);
        offset += strlen(dirp->d_name) + 1;     /* + zero byte separator */
    }

    strcpy(sendbuffer + offset, CFD_TERMINATOR);
    SendTransaction(&conn->conn_info, sendbuffer, offset + 2 + strlen(CFD_TERMINATOR), CF_DONE);
    DirClose(dirh);
    return 0;
}

/**************************************************************/

static int CfSecOpenDirectory(ServerConnectionState *conn, char *sendbuffer, char *dirname)
{
    Dir *dirh;
    const struct dirent *dirp;
    int offset, cipherlen;
    char out[CF_BUFSIZE];

    if (!IsAbsoluteFileName(dirname))
    {
        sprintf(sendbuffer, "BAD: request to access a non-absolute filename\n");
        cipherlen = EncryptString(conn->encryption_type, sendbuffer, out, conn->session_key, strlen(sendbuffer) + 1);
        SendTransaction(&conn->conn_info, out, cipherlen, CF_DONE);
        return -1;
    }

    if ((dirh = DirOpen(dirname)) == NULL)
    {
        Log(LOG_LEVEL_VERBOSE, "Couldn't open dir %s", dirname);
        snprintf(sendbuffer, CF_BUFSIZE, "BAD: cfengine, couldn't open dir %s\n", dirname);
        cipherlen = EncryptString(conn->encryption_type, sendbuffer, out, conn->session_key, strlen(sendbuffer) + 1);
        SendTransaction(&conn->conn_info, out, cipherlen, CF_DONE);
        return -1;
    }

/* Pack names for transmission */

    memset(sendbuffer, 0, CF_BUFSIZE);

    offset = 0;

    for (dirp = DirRead(dirh); dirp != NULL; dirp = DirRead(dirh))
    {
        if (strlen(dirp->d_name) + 1 + offset >= CF_BUFSIZE - CF_MAXLINKSIZE)
        {
            cipherlen = EncryptString(conn->encryption_type, sendbuffer, out, conn->session_key, offset + 1);
            SendTransaction(&conn->conn_info, out, cipherlen, CF_MORE);
            offset = 0;
            memset(sendbuffer, 0, CF_BUFSIZE);
            memset(out, 0, CF_BUFSIZE);
        }

        strncpy(sendbuffer + offset, dirp->d_name, CF_MAXLINKSIZE);
        /* + zero byte separator */
        offset += strlen(dirp->d_name) + 1;
    }

    strcpy(sendbuffer + offset, CFD_TERMINATOR);

    cipherlen =
        EncryptString(conn->encryption_type, sendbuffer, out, conn->session_key, offset + 2 + strlen(CFD_TERMINATOR));
    SendTransaction(&conn->conn_info, out, cipherlen, CF_DONE);
    DirClose(dirh);
    return 0;
}

/***************************************************************/

static void Terminate(ConnectionInfo *connection)
{
    char buffer[CF_BUFSIZE];

    memset(buffer, 0, CF_BUFSIZE);

    strcpy(buffer, CFD_TERMINATOR);

    if (SendTransaction(connection, buffer, strlen(buffer) + 1, CF_DONE) == -1)
    {
        Log(LOG_LEVEL_VERBOSE, "Unable to reply with terminator. (send: %s)", GetErrorStr());
    }
}

/***************************************************************/

void DeleteAuthList(Auth *ap)
{
    if (ap != NULL)
    {
        DeleteAuthList(ap->next);
        ap->next = NULL;

        DeleteItemList(ap->accesslist);
        DeleteItemList(ap->maproot);
        free(ap->path);
        free((char *) ap);
    }
}

/***************************************************************/
/* Level 5                                                     */
/***************************************************************/

static int OptionFound(char *args, char *pos, char *word)
/*
 * Returns true if the current position 'pos' in buffer
 * 'args' corresponds to the word 'word'.  Words are
 * separated by spaces.
 */
{
    size_t len;

    if (pos < args)
    {
        return false;
    }

/* Single options do not have to have spaces between */

    if ((strlen(word) == 2) && (strncmp(pos, word, 2) == 0))
    {
        return true;
    }

    len = strlen(word);

    if (strncmp(pos, word, len) != 0)
    {
        return false;
    }

    if (pos == args)
    {
        return true;
    }
    else if ((*(pos - 1) == ' ') && ((pos[len] == ' ') || (pos[len] == '\0')))
    {
        return true;
    }
    else
    {
        return false;
    }
}

/**************************************************************/

static void RefuseAccess(ServerConnectionState *conn, int size, char *errmesg)
{
    char *hostname, *username, *ipaddr;
    char *def = "?";

    if (strlen(conn->hostname) == 0)
    {
        hostname = def;
    }
    else
    {
        hostname = conn->hostname;
    }

    if (strlen(conn->username) == 0)
    {
        username = def;
    }
    else
    {
        username = conn->username;
    }

    if (strlen(conn->ipaddr) == 0)
    {
        ipaddr = def;
    }
    else
    {
        ipaddr = conn->ipaddr;
    }

    char sendbuffer[CF_BUFSIZE];
    snprintf(sendbuffer, CF_BUFSIZE, "%s", CF_FAILEDSTR);
    SendTransaction(&conn->conn_info, sendbuffer, size, CF_DONE);

    Log(LOG_LEVEL_INFO, "From (host=%s,user=%s,ip=%s)", hostname, username, ipaddr);

    if (strlen(errmesg) > 0)
    {
        if (SV.logconns)
        {
            Log(LOG_LEVEL_INFO, "REFUSAL of request from connecting host: (%s)", errmesg);
        }
        else
        {
            Log(LOG_LEVEL_VERBOSE, "REFUSAL of request from connecting host: (%s)", errmesg);
        }
    }
}

/***************************************************************/

static int TransferRights(char *filename, ServerFileGetState *args, struct stat *sb)
{
#ifdef __MINGW32__
    SECURITY_DESCRIPTOR *secDesc;
    SID *ownerSid;

    if (GetNamedSecurityInfo
        (filename, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, (PSID *) & ownerSid, NULL, NULL, NULL,
         (void **)&secDesc) == ERROR_SUCCESS)
    {
        if (IsValidSid((args->connect)->sid) && EqualSid(ownerSid, (args->connect)->sid))
        {
            Log(LOG_LEVEL_DEBUG, "Caller '%s' is the owner of the file", (args->connect)->username);
        }
        else
        {
            // If the process doesn't own the file, we can access if we are
            // root AND granted root map

            LocalFree(secDesc);

            if (args->connect->maproot)
            {
                Log(LOG_LEVEL_VERBOSE, "Caller '%s' not owner of '%s', but mapping privilege",
                      (args->connect)->username, filename);
                return true;
            }
            else
            {
                Log(LOG_LEVEL_VERBOSE, "Remote user denied right to file '%s' (consider maproot?)", filename);
                return false;
            }
        }

        LocalFree(secDesc);
    }
    else
    {
        Log(LOG_LEVEL_ERR, "Could not retreive existing owner of '%s'. (GetNamedSecurityInfo)", filename);
        return false;
    }

#else

    uid_t uid = (args->connect)->uid;

    if ((uid != 0) && (!args->connect->maproot))    /* should remote root be local root */
    {
        if (sb->st_uid == uid)
        {
            Log(LOG_LEVEL_DEBUG, "Caller '%s' is the owner of the file", (args->connect)->username);
        }
        else
        {
            if (sb->st_mode & S_IROTH)
            {
                Log(LOG_LEVEL_DEBUG, "Caller %s not owner of the file but permission granted", (args->connect)->username);
            }
            else
            {
                Log(LOG_LEVEL_DEBUG, "Caller '%s' is not the owner of the file", (args->connect)->username);
                Log(LOG_LEVEL_VERBOSE, "Remote user denied right to file '%s' (consider maproot?)", filename);
                return false;
            }
        }
    }
#endif

    return true;
}

/***************************************************************/

static void AbortTransfer(ConnectionInfo *connection, char *filename)
{
    Log(LOG_LEVEL_VERBOSE, "Aborting transfer of file due to source changes");

    char sendbuffer[CF_BUFSIZE];
    snprintf(sendbuffer, CF_BUFSIZE, "%s%s: %s", CF_CHANGEDSTR1, CF_CHANGEDSTR2, filename);

    if (SendTransaction(connection, sendbuffer, 0, CF_DONE) == -1)
    {
        Log(LOG_LEVEL_VERBOSE, "Send failed in GetFile. (send: %s)", GetErrorStr());
    }
}

/***************************************************************/

static void FailedTransfer(ConnectionInfo *connection)
{
    Log(LOG_LEVEL_VERBOSE, "Transfer failure");

    char sendbuffer[CF_BUFSIZE];

    snprintf(sendbuffer, CF_BUFSIZE, "%s", CF_FAILEDSTR);

    if (SendTransaction(connection, sendbuffer, 0, CF_DONE) == -1)
    {
        Log(LOG_LEVEL_VERBOSE, "Send failed in GetFile. (send: %s)", GetErrorStr());
    }
}

/***************************************************************/

static void ReplyNothing(ServerConnectionState *conn)
{
    char buffer[CF_BUFSIZE];

    snprintf(buffer, CF_BUFSIZE, "Hello %s (%s), nothing relevant to do here...\n\n", conn->hostname, conn->ipaddr);

    if (SendTransaction(&conn->conn_info, buffer, 0, CF_DONE) == -1)
    {
        Log(LOG_LEVEL_ERR, "Unable to send transaction. (send: %s)", GetErrorStr());
    }
}

/***************************************************************/

static int CheckStoreKey(ServerConnectionState *conn, RSA *key)
{
    RSA *savedkey;
    char udigest[CF_MAXVARSIZE];
    char buffer[EVP_MAX_MD_SIZE * 4];

    snprintf(udigest, CF_MAXVARSIZE - 1, "%s", HashPrintSafe(CF_DEFAULT_DIGEST, conn->digest, buffer));

    if ((savedkey = HavePublicKey(conn->username, MapAddress(conn->ipaddr), udigest)))
    {
        Log(LOG_LEVEL_VERBOSE, "A public key was already known from %s/%s - no trust required", conn->hostname,
              conn->ipaddr);

        Log(LOG_LEVEL_VERBOSE, "Adding IP %s to SkipVerify - no need to check this if we have a key", conn->ipaddr);
        IdempPrependItem(&SV.skipverify, MapAddress(conn->ipaddr), NULL);

        if ((BN_cmp(savedkey->e, key->e) == 0) && (BN_cmp(savedkey->n, key->n) == 0))
        {
            Log(LOG_LEVEL_VERBOSE, "The public key identity was confirmed as %s@%s", conn->username, conn->hostname);
            SendTransaction(&conn->conn_info, "OK: key accepted", 0, CF_DONE);
            RSA_free(savedkey);
            return true;
        }
    }

/* Finally, if we're still here, we should consider trusting a new key ... */

    if ((SV.trustkeylist != NULL) && (IsMatchItemIn(conn->ctx, SV.trustkeylist, MapAddress(conn->ipaddr))))
    {
        Log(LOG_LEVEL_VERBOSE, "Host %s/%s was found in the list of hosts to trust", conn->hostname, conn->ipaddr);
        conn->trust = true;
        /* conn->maproot = false; ?? */
        SendTransaction(&conn->conn_info, "OK: unknown key was accepted on trust", 0, CF_DONE);
        SavePublicKey(conn->username, udigest, key);
        return true;
    }
    else
    {
        Log(LOG_LEVEL_VERBOSE, "No previous key found, and unable to accept this one on trust");
        SendTransaction(&conn->conn_info, "BAD: key could not be accepted on trust", 0, CF_DONE);
        return false;
    }
}

/*
 * This function sends "ACK" to the client and then waits for the client to start the TLS connection.
 */
static int DoStartTLS(ServerConnectionState *connection)
{
    return ServerStartTLS(&connection->conn_info);
}

/***************************************************************/
/* Toolkit/Class: conn                                         */
/***************************************************************/

static ServerConnectionState *NewConn(EvalContext *ctx, int sd)
{
    ServerConnectionState *conn;
    struct sockaddr addr;
    socklen_t size = sizeof(addr);

    if (getsockname(sd, &addr, &size) == -1)
    {
       return NULL;
    }

    conn = xmalloc(sizeof(ServerConnectionState));

    conn->ctx = ctx;
    conn->conn_info.type = CF_PROTOCOL_CLASSIC;
    conn->conn_info.sd = sd;
    conn->conn_info.ssl = NULL;
    conn->id_verified = false;
    conn->rsa_auth = false;
    conn->trust = false;
    conn->hostname[0] = '\0';
    conn->ipaddr[0] = '\0';
    conn->username[0] = '\0';
    conn->session_key = NULL;
    conn->encryption_type = 'c';

    Log(LOG_LEVEL_DEBUG, "New socket %d", sd);

    return conn;
}

/***************************************************************/

static void DeleteConn(ServerConnectionState *conn)
{
    /* Sockets should have already been closed by the client, so we are just
     * making sure here in case an error occured. */
    switch(conn->conn_info.type)
    {
    case CF_PROTOCOL_CLASSIC:
        cf_closesocket(conn->conn_info.sd);
        break;
    case CF_PROTOCOL_TLS:
        SSL_shutdown(conn->conn_info.ssl);
        SSL_free(conn->conn_info.ssl);
        cf_closesocket(conn->conn_info.sd);
        break;
    default:
        UnexpectedError("DeleteConn: ProtocolVersion %d!",
                        conn->conn_info.type);
    }

    if (conn->session_key != NULL)
    {
        free(conn->session_key);
    }
    if (conn->ipaddr != NULL)
    {
        if (!ThreadLock(cft_count))
        {
            return;
        }

        DeleteItemMatching(conn->ctx, &SV.connectionlist, MapAddress(conn->ipaddr));

        if (!ThreadUnlock(cft_count))
        {
            return;
        }
    }

    *conn = (ServerConnectionState) {0};
    free(conn);
}

static int cfscanf(char *in, int len1, int len2, char *out1, char *out2, char *out3)
{
    int len3 = 0;
    char *sp;

    sp = in;
    memcpy(out1, sp, len1);
    out1[len1] = '\0';

    sp += len1 + 1;
    memcpy(out2, sp, len2);

    sp += len2 + 1;
    len3 = strlen(sp);
    memcpy(out3, sp, len3);
    out3[len3] = '\0';

    return (len1 + len2 + len3 + 2);
}
