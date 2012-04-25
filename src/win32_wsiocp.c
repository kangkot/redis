/*
 * Copyright (c) Microsoft Open Technologies, Inc.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License.  You may obtain a copy of
 * the License at http://www.apache.org/licenses/LICENSE-2.0.
 *
 * THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING
 * WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE,
 * FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
 *
 * See the Apache License, Version 2.0 for specific language governing
 * permissions and limitations under the License.
 */

#include "ae.h"
#include "win32fixes.h"
#include "zmalloc.h"
#include <mswsock.h>
#include <Guiddef.h>
#include "win32_wsiocp.h"


static void *iocpState;
static HANDLE iocph;
static fnGetSockState * aeGetSockState;
static fnDelSockState * aeDelSockState;

static LPFN_ACCEPTEX acceptex;
static LPFN_GETACCEPTEXSOCKADDRS getaddrs;

#define SUCCEEDED_WITH_IOCP(result)                        \
  ((result) || (GetLastError() == ERROR_IO_PENDING))

/* for zero length reads use shared buf */
static DWORD wsarecvflags;
static char zreadchar[1];


/* queue an accept with a new socket */
int aeWinQueueAccept(SOCKET listensock) {
    aeSockState *sockstate;
    aeSockState *accsockstate;
    DWORD result, bytes;
    SOCKET acceptsock;
    aacceptreq * areq;

    if ((sockstate = aeGetSockState(iocpState, listensock)) == NULL) {
        errno = WSAEINVAL;
        return -1;
    }

    acceptsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (acceptsock == INVALID_SOCKET) {
        errno = WSAEINVAL;
        return -1;
    }

    accsockstate = aeGetSockState(iocpState, acceptsock);
    if (accsockstate == NULL) {
        errno = WSAEINVAL;
        return -1;
    }

    accsockstate->masks = SOCKET_ATTACHED;
    /* keep accept socket in buf len until accepted */
    areq = (aacceptreq *)zmalloc(sizeof(aacceptreq));
    memset(areq, 0, sizeof(aacceptreq));
    areq->buf = (char *)zmalloc(sizeof(struct sockaddr_storage) * 2 + 64);
    areq->accept = acceptsock;
    areq->next = NULL;

    result = acceptex(listensock, acceptsock,
                            areq->buf, 0,
                            sizeof(struct sockaddr_storage),
                            sizeof(struct sockaddr_storage),
                            &bytes, &areq->ov);
    if (SUCCEEDED_WITH_IOCP(result)){
        sockstate->masks |= ACCEPT_PENDING;
    } else {
        errno = WSAGetLastError();
        sockstate->masks &= ~ACCEPT_PENDING;
        closesocket(acceptsock);
        accsockstate->masks = 0;
        zfree(areq);
        return -1;
    }

    return TRUE;
}

/* listen using extension function to get faster accepts */
int aeWinListen(SOCKET sock, int backlog) {
    aeSockState *sockstate;
    const GUID wsaid_acceptex = WSAID_ACCEPTEX;
    const GUID wsaid_acceptexaddrs = WSAID_GETACCEPTEXSOCKADDRS;
    DWORD result, bytes;

    if ((sockstate = aeGetSockState(iocpState, sock)) == NULL) {
        errno = WSAEINVAL;
        return SOCKET_ERROR;
    }

    aeWinSocketAttach(sock);
    sockstate->masks |= LISTEN_SOCK;

    result = WSAIoctl(sock,
                    SIO_GET_EXTENSION_FUNCTION_POINTER,
                    (void *)&wsaid_acceptex,
                    sizeof(GUID),
                    &acceptex,
                    sizeof(LPFN_ACCEPTEX),
                    &bytes,
                    NULL,
                    NULL);

    if (result == SOCKET_ERROR) {
        acceptex = NULL;
        return SOCKET_ERROR;
    }
    result = WSAIoctl(sock,
                    SIO_GET_EXTENSION_FUNCTION_POINTER,
                    (void *)&wsaid_acceptexaddrs,
                    sizeof(GUID),
                    &getaddrs,
                    sizeof(LPFN_GETACCEPTEXSOCKADDRS),
                    &bytes,
                    NULL,
                    NULL);

    if (result == SOCKET_ERROR) {
        getaddrs = NULL;
        return SOCKET_ERROR;
    }

    if (listen(sock, backlog) == 0) {
        if (aeWinQueueAccept(sock) == -1) {
            return SOCKET_ERROR;
        }
    }

    return 0;
}

/* return the queued accept socket */
int aeWinAccept(int fd, struct sockaddr *sa, socklen_t *len) {
    aeSockState *sockstate;
    int acceptsock;
    int result;
    SOCKADDR *plocalsa;
    SOCKADDR *premotesa;
    int locallen, remotelen;
    aacceptreq * areq;

    if ((sockstate = aeGetSockState(iocpState, fd)) == NULL) {
        errno = WSAEINVAL;
        return SOCKET_ERROR;
    }


    areq = sockstate->reqs;
    if (areq == NULL) {
        errno = WSAEINVAL;
        return SOCKET_ERROR;
    }

    sockstate->reqs = areq->next;

    acceptsock = areq->accept;

    result = setsockopt(acceptsock,
                        SOL_SOCKET,
                        SO_UPDATE_ACCEPT_CONTEXT,
                        (char*)&fd,
                        sizeof(fd));
    if (result == SOCKET_ERROR) {
        errno = WSAGetLastError();
        return SOCKET_ERROR;
    }

    locallen = *len;
    getaddrs(areq->buf,
                    0,
                    sizeof(struct sockaddr_storage),
                    sizeof(struct sockaddr_storage),
                    &plocalsa, &locallen,
                    &premotesa, &remotelen);

    locallen = remotelen < *len ? remotelen : *len;
    memcpy(sa, premotesa, locallen);
    *len = locallen;

    aeWinSocketAttach(acceptsock);

    zfree(areq);

    /* queue another accept */
    if (aeWinQueueAccept(fd) == -1) {
        return SOCKET_ERROR;
    }

    return acceptsock;
}


/* after doing read caller needs to call done
 * so that we can continue to check for read events.
 * This is not necessary if caller will delete read events */
int aeWinReceiveDone(int fd) {
    aeSockState *sockstate;
    int result;
    WSABUF zreadbuf;

    if ((sockstate = aeGetSockState(iocpState, fd)) == NULL) {
        errno = WSAEINVAL;
        return -1;
    }
    if ((sockstate->masks & SOCKET_ATTACHED) == 0) {
        return 0;
    }

    /* use zero length read with overlapped to get notification
     of when data is available */
    memset(&sockstate->ov_read, 0, sizeof(sockstate->ov_read));

    zreadbuf.buf = zreadchar;
    zreadbuf.len = 0;
    result = WSARecv((SOCKET)fd,
                     &zreadbuf,
                     1,
                     NULL,
                     &wsarecvflags,
                     &sockstate->ov_read,
                     NULL);
    if (SUCCEEDED_WITH_IOCP(result == 0)){
        sockstate->masks |= READ_QUEUED;
    } else {
        errno = WSAGetLastError();
        sockstate->masks &= ~READ_QUEUED;
        return -1;
    }
    return 0;
}

/* wrapper for send
 * enables use of WSA Send to get IOCP notification of completion.
 * returns -1  with errno = WSA_IO_PENDING if callback will be invoked later */
int aeWinSocketSend(int fd, char *buf, int len, int flags,
                    void *eventLoop, void *client, void *data, void *proc) {
    aeSockState *sockstate;
    int result;
    asendreq *areq;

    sockstate = aeGetSockState(iocpState, fd);
    /* if not an async socket, do normal send */
    if (sockstate == NULL ||
        (sockstate->masks & SOCKET_ATTACHED) == 0 ||
        proc == NULL) {
        result = send((SOCKET)fd, buf, len, flags);
        if (result == SOCKET_ERROR) {
            errno = WSAGetLastError();
        }
        return result;
    }

    /* use overlapped structure to send using IOCP */
    areq = (asendreq *)zmalloc(sizeof(asendreq));
    memset(areq, 0, sizeof(asendreq));
    areq->wbuf.len = len;
    areq->wbuf.buf = buf;
    areq->eventLoop = (aeEventLoop *)eventLoop;
    areq->req.client = client;
    areq->req.data = data;
    areq->req.len = len;
    areq->req.buf = buf;
    areq->proc = (aeFileProc *)proc;

    result = WSASend((SOCKET)fd,
                    &areq->wbuf,
                    1,
                    NULL,
                    flags,
                    &areq->ov,
                    NULL);

    if (SUCCEEDED_WITH_IOCP(result == 0)){
        errno = WSA_IO_PENDING;
        sockstate->wreqs++;
    } else {
        errno = WSAGetLastError();
        zfree(areq);
    }
    return SOCKET_ERROR;
}

/* for each asynch socket, need to associate completion port */
int aeWinSocketAttach(int fd) {
    DWORD yes = 1;
    aeSockState *sockstate;

    if ((sockstate = aeGetSockState(iocpState, fd)) == NULL) {
        errno = WSAEINVAL;
        return -1;
    }

    /* Set the socket to nonblocking mode */
    if (ioctlsocket((SOCKET)fd, FIONBIO, &yes) == SOCKET_ERROR) {
        errno = WSAGetLastError();
        return -1;
    }

    /* Make the socket non-inheritable */
    if (!SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, 0)) {
        errno = WSAGetLastError();
        return -1;
    }

    /* Associate it with the I/O completion port. */
    /* Use socket as completion key. */
    if (CreateIoCompletionPort((HANDLE)fd,
                                iocph,
                                (ULONG_PTR)fd,
                                0) == NULL) {
        errno = WSAGetLastError();
        return -1;
    }
    sockstate->masks = SOCKET_ATTACHED;
    sockstate->wreqs = 0;

    return 0;
}

/* when closing socket, need to unassociate completion port */
int aeWinSocketDetach(int fd, int shutd) {
    aeSockState *sockstate;
    char rbuf[100];

    if ((sockstate = aeGetSockState(iocpState, fd)) == NULL) {
        errno = WSAEINVAL;
        return -1;
    }

    if (shutd == 1) {
        if (shutdown(fd, SD_SEND) != SOCKET_ERROR) {
            /* read data until no more or error to ensure shutdown completed */
            while (1) {
                int rc = recv(fd, rbuf, 100, 0);
                if (rc == 0 || rc == SOCKET_ERROR) break;
            }
        } else {
            int err = WSAGetLastError();
        }
    }
    sockstate->masks &= ~(SOCKET_ATTACHED | AE_WRITABLE | AE_READABLE);
    if (sockstate->wreqs == 0 && (sockstate->masks & READ_QUEUED) == 0) {
        // safe to delete sockstate
        aeDelSockState(iocpState, sockstate);
    }
    return 0;
}

void aeWinInit(void *state, HANDLE iocp, fnGetSockState *getSockState,
                                        fnDelSockState *delSockState) {
    iocpState = state;
    iocph = iocp;
    aeGetSockState = getSockState;
    aeDelSockState = delSockState;
}

void aeWinCleanup() {
    iocpState = NULL;
}