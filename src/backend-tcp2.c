/*
 * This file is part of the LAIK library.
 * Copyright (c) 2020 Josef Weidendorfer <Josef.Weidendorfer@gmx.de>
 *
 * LAIK is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, version 3 or later.
 *
 * LAIK is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Planned design (to be implemented / updated)
 *
 * The protocol used at TCP level among processes should enable easy debugging
 * and playing with ideas (eg. via 'nc'/'telnet'), performance is low priority.
 * Thus, it is based on text and line separation.
 *
 * When messages end in <value> at end of line, it can be send as
 * - "+<payload length in bytes> R\n<raw bytes>\n"
 * - "+<payload lentgh in bytes> H\n" and multiple lines of
 *   " [(<offset>)] [<hex>*] [# <comment>]\n"
 * - for data of specific type, <hex> can be replaced by element value converted
 *   into ASCII representation, providing the element type instead of H, e.g.
 *   D/F/U32/U64/I32/I64 for double, float, (un)signed 32/64 bit, respectively
 *
 * Messages can be preceded by lines starting with "# ...\n" as comments, which
 * are ignored but may be written as log at receiver side for debugging
 *
 * Startup:
 * - home process (location ID 0) is the process started on LAIK_TCP2_HOST
 *   (default: localhost) which aquired LAIK_TCP2_PORT for listening
 * - other processes register with home process to join
 * - home process waits for LAIK_SIZE (default: 1) processes to join before
 *   finishing initialization and giving control to application
 *
 * Registration:
 * - open own listening port (usually randomly assigned by OS) at <myport>
 * - connect to home process; this may block until home can accept connections
 * - send "register <mylocation> <myhost> <myport>\n"
 *     - <mylocation> can be any string, but should be unique
 *     - if <myhost> is not specified, it is identified as connecting peer
 * - home sends an ID line for the new assigned id of registering process
 *     "id <id> <location> <host> <port>\n"
 * - afterwards, home sends the following message types in arbitrary order:
 *   - further ID lines, for each registered process
 *   - config lines "config <key> <value>\n"
 *   - serialized objects "object <type> <name> <version> <refcount> <value>\n"
 * - at end: home sends current compute phase "phase <phaseid> <iteration>\n"
 * - give back control to application, connection can stay open
 *
 * Elasticity:
 * - LAIK checks backend for processes wanting to join at compute phase change
 * - processes tell master about reached phase and ask for new IDs
 *     "resize <phaseid> <maxid>"
 * - master answers
 *   - new ID lines of processes joining
 *   - ids to be removed: "remove <id>"
 *   - finishes with "done"
 * - control given back to application, to process resize request
 * 
 * Data exchange:
 * - always done directly between 2 processes, using any existing connection
 * - if no connection exists yet
 *     - receiver always waits to be connected
 *     - sender connects to listening port of receiver, sends "id <id>\n"
 * - sender sends "data <container name> <start index> <element count> <value>"
 * - connections can be used bidirectionally
  * 
 * Sync:
 * - two phases:
 *   - send changed objects to home process
 *   - receiving changes from home process
 * - start with "sync <id>\n"
 * - multiple "object <type> <name> <version> <refcount>[ <value>]\n"
 *   sending <value> is optional if only <refcount> changes
 * - end with "done\n"
 * - objects may be released if all <refcounts> are 0
 *
 * Deregistration: todo
 *
 * External commands: todo
 */


#ifdef USE_TCP2

#include "laik-internal.h"
#include "laik-backend-tcp2.h"

#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
// for VSC to see def of addrinfo
#ifndef __USE_XOPEN2K
#define __USE_XOPEN2K 1
#endif
#include <netdb.h>

// defaults
#define TCP2_PORT 7777

#define MAX_PEERS 256
#define MAX_FDS 256
// receive buffer length
#define RBUF_LEN 256

// forward decl
void tcp2_exec(Laik_ActionSeq* as);
void tcp2_sync(Laik_KVStore* kvs);

typedef struct _InstData InstData;

// C guarantees that unset function pointers are NULL
static Laik_Backend laik_backend = {
    .name = "Dynamic TCP2 Backend",
    .exec = tcp2_exec,
    .sync = tcp2_sync
};

static Laik_Instance* instance = 0;

// structs for instance

// communicating peer
// can be connected (fd >=0) or not
typedef struct {
    int fd;         // -1 if not connected
    int port;       // port to connect to at host
    char* host;     // remote host, if 0 localhost
    char* location; // location string of peer

    // data we are currently receiving from peer
    int rcount;    // element count in receive
    int relemsize; // expected byte count per element
    int roff;      // receive offset
    Laik_Mapping* rmap; // mapping to write received data to
    Laik_Slice* rslc; // slice to write received data to
    Laik_Index ridx; // index representing receive progress
    Laik_ReductionOperation rro; // reduction with existing value

    // allowed to send data to peer?
    int scount;    // element count allowed to send, 0 if not
    int selemsize; // byte count expected per element
} Peer;

// registrations for active fds in event loop
typedef void (*loop_cb_t)(InstData* d, int fd);
typedef struct {
    int lid;          // location ID of peer
    loop_cb_t cb;
    int rbuf_used;
    char* rbuf;
} FDState;

struct _InstData {
    int mylid;        // my location ID
    char* host;       // my hostname
    char* location;   // my location
    int listenfd;     // file descriptor for listening to connections
    int listenport;   // port we listen at (random unless master)
    int maxid;        // highest seen id
    int phase;        // current phase

    // event loop
    int maxfds;       // highest fd in rset
    fd_set rset;      // read set for select
    int exit;         // set to exit event loop
    FDState fds[MAX_FDS];

    int peers;        // number of active peers, can be 0 only at master
    Peer peer[0];
};


// helpers for send/receive of LAIK containers

// index traversal over slice
// return true if index was successfully incremented, false if traversal done
// (copied from src/layout.c)
static
bool next_lex(Laik_Slice* slc, Laik_Index* idx)
{
    idx->i[0]++;
    if (idx->i[0] < slc->to.i[0]) return true;
    if (slc->space->dims == 1) return false;

    idx->i[1]++;
    idx->i[0] = slc->from.i[0];
    if (idx->i[1] < slc->to.i[1]) return true;
    if (slc->space->dims == 2) return false;

    idx->i[2]++;
    idx->i[1] = slc->from.i[1];
    if (idx->i[2] < slc->to.i[2]) return true;
    return false;
}

static
char* istr(int dims, Laik_Index* idx)
{
    static char str[50];

    if (dims == 1)
        sprintf(str, "%llu", (unsigned long long) idx->i[0]);
    if (dims == 2)
        sprintf(str, "%llu/%llu",
                (unsigned long long) idx->i[0],
                (unsigned long long) idx->i[1]);
    if (dims == 3)
        sprintf(str, "%llu/%llu/%llu",
                (unsigned long long) idx->i[0],
                (unsigned long long) idx->i[1],
                (unsigned long long) idx->i[2]);
    return str;
}


// event loop functions

void add_rfd(InstData* d, int fd, loop_cb_t cb)
{
    assert(fd < MAX_FDS);
    assert(d->fds[fd].cb == 0);

    FD_SET(fd, &d->rset);
    if (fd > d->maxfds) d->maxfds = fd;
    d->fds[fd].cb = cb;
    d->fds[fd].lid = -1;
    d->fds[fd].rbuf = malloc(RBUF_LEN);
    d->fds[fd].rbuf_used = 0;
}

void rm_rfd(InstData* d, int fd)
{
    assert(fd < MAX_FDS);
    assert(d->fds[fd].cb != 0);

    FD_CLR(fd, &d->rset);
    if (fd == d->maxfds)
        while(!FD_ISSET(d->maxfds, &d->rset)) d->maxfds--;
    d->fds[fd].cb = 0;
    free(d->fds[fd].rbuf);
    d->fds[fd].rbuf = 0;
}

void run_loop(InstData* d)
{
    d->exit = 0;
    while(d->exit == 0) {
        fd_set rset = d->rset;
        if (select(d->maxfds+1, &rset, 0, 0, 0) >= 0) {
            for(int i = 0; i <= d->maxfds; i++)
                if (FD_ISSET(i, &rset)) {
                    assert(d->fds[i].cb != 0);
                    (d->fds[i].cb)(d, i);
                }
        }
    }
}


// helper functions

// return true if hostname maps to localhost by binding a socket at arbitrary port
bool check_local(char* host)
{
    struct addrinfo hints, *info, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int ret;
    if ((ret = getaddrinfo(host, 0, &hints, &info)) != 0) {
        // host not found: not fatal here
        laik_log(1, "TCP2 check_local - host %s not found", host);
        return false;
    }
    int fd = -1;
    for(p = info; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == -1) continue;
        if (p->ai_addr->sa_family == AF_INET)
            ((struct sockaddr_in*)(p->ai_addr))->sin_port = 0;
        else if (p->ai_addr->sa_family == AF_INET6)
            ((struct sockaddr_in6*)(p->ai_addr))->sin6_port = 0;
        else {
            close(fd);
            continue;
        }
        if (bind(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
    }
    if (p) close(fd);
    return (p != 0);
}

// forward decl
void got_bytes(InstData* d, int fd);
void send_cmd(InstData* d, int lid, char* cmd);

// make sure we have an open connection to peer <lid>
// if not, connect to listening port of peer, and announce mylid
void ensure_conn(InstData* d, int lid)
{
    assert(lid < MAX_PEERS);
    if (d->peer[lid].fd >= 0) return; // connected

    assert(d->peer[lid].port >= 0);
    char port[20];
    sprintf(port, "%d", d->peer[lid].port);

    struct addrinfo hints, *info, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int ret;
    if ((ret = getaddrinfo(d->peer[lid].host, port, &hints, &info)) != 0) {
        laik_log(LAIK_LL_Panic, "TCP2 host %s not found - getaddrinfo %s",
                 d->peer[lid].host, gai_strerror(ret));
        exit(1);
    }
    int fd = -1;
    for(p = info; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == -1) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
    }
    if (p == 0) {
        laik_log(LAIK_LL_Panic, "TCP2 cannot connect to LID %d (host %s, port %d)",
                 lid, d->peer[lid].host, d->peer[lid].port);
        exit(1);
    }
    d->peer[lid].fd = fd;
    add_rfd(d, fd, got_bytes);
    d->fds[fd].lid = lid;
    laik_log(1, "TCP2 connected to LID %d (host %s, port %d)",
             lid, d->peer[lid].host, d->peer[lid].port);

    if (d->mylid >= 0) {
        // make myself known to peer: send my location id
        char msg[20];
        sprintf(msg, "myid %d", d->mylid);
        send_cmd(d, lid, msg);
    }
}

// send command <cmd> to peer <lid>
// if <lid> is negative, this specifies the FD instead as (-<lid>)
//   (receiver has no location id assigned yet)
void send_cmd(InstData* d, int lid, char* cmd)
{
    int fd = -lid;
    if (lid >= 0) {
        ensure_conn(d, lid);
        fd = d->peer[lid].fd;
    }
    int len = strlen(cmd);
    laik_log(1, "TCP2 Sent cmd '%s' (len %d) to locID %d (FD %d)\n",
             cmd, len, lid, fd);

    // write cmd + NL (cope with partial writes and errors)
    int res, written = 0;
    while(written < len) {
        res = write(fd, cmd + written, len - written);
        if (res < 0) break;
        written += res;
    }
    if (res >= 0) while((res = write(fd, "\n", 1)) == 0);
    if (res < 0) {
        int e = errno;
        laik_log(LAIK_LL_Warning, "TCP2 write error on FD %d: %s\n",
                 fd, strerror(e));
    }
}

// "data" command received
// return false if command cannot be processed yet, no matching receive
void got_data(InstData* d, int lid, char* msg)
{
    // data <len> <hexbyte> ...
    char cmd[20];
    int len, i;
    if (sscanf(msg, "%20s %d %n", cmd, &len, &i) < 2) {
        laik_log(LAIK_LL_Panic, "cannot parse data command '%s'", msg);
        return;
    }

    Peer* p = &(d->peer[lid]);
    if ((p->rcount == 0) || (p->rcount == p->roff)) {
        laik_log(LAIK_LL_Warning, "TCP2 ignoring data from LID %d without send permission", lid);
        return;
    }

    // assume only one element per data command
    assert(p->relemsize == len);
    Laik_Mapping* m = p->rmap;
    assert(m != 0);
    Laik_Layout* ll = m->layout;
    int64_t off = ll->offset(ll, m->layoutSection, &(p->ridx));
    char* idxPtr = m->start + off * p->relemsize;

    // position string for check
    char pstr[70];
    int dims = p->rslc->space->dims;
    int s = sprintf(pstr, "(%d:%s)", p->roff, istr(dims, &(p->ridx)));

    if (msg[i] == '(') {
        assert(strncmp(msg+i, pstr, s) == 0);
        i += s;
        while(msg[i] && (msg[i] == ' ')) i++;
    }

    char data_in[100];
    assert(len < 100);
    int l = 0;

    // parse hex bytes
    unsigned char c;
    int v = 0;
    while(1) {
        c = msg[i++];
        if ((c == ' ') || (c == 0)) {
            assert(l < len);
            data_in[l++] = v;
            v = 0;
            if ((c == 0) || (l == len)) break;
            continue;
        }
        assert((c >= '0') && (c <= 'f'));
        assert((c <= '9') || (c >= 'a'));
        v = 16 * v + ((c > '9') ? c - 'a' + 10 : c - '0');
    }
    assert(l == len);

    assert(l == p->relemsize);
    if (p->rro == LAIK_RO_None)
        memcpy(idxPtr, data_in, len);
    else {
        Laik_Type* t = p->rmap->data->type;
        assert(t->reduce);
        (t->reduce)(idxPtr, idxPtr, data_in, 1, p->rro);
    }

    if (len == 8) laik_log(1, " pos %s: in %f res %f\n", pstr, *((double*)data_in), *((double*)idxPtr));

    p->roff++;
    bool inTraversal = next_lex(p->rslc, &(p->ridx));
    assert(inTraversal == (p->roff < p->rcount));

    laik_log(1, "TCP2 got data, len %d, received %d/%d",
             len, p->roff, p->rcount);

    if (p->roff == p->rcount)
        d->exit = 1;

    return;
}

// a command was received from a peer, and should be processed
// return false if command cannot be processed yet
//   this only happens with "data" command without matching receive
void got_cmd(InstData* d, int fd, char* msg, int len)
{
    int lid = d->fds[fd].lid;
    laik_log(1, "TCP2 Got cmd '%s' (len %d) from locID %d (FD %d)\n",
            msg, len, lid, fd);

    // first part of commands are accepted without assigned ID

    if (msg[0] == 'r') {
        // register <location> <host> <port>

        // ignore if not master
        if (d->mylid != 0) {
            laik_log(LAIK_LL_Warning, "ignoring register command '%s', not master", msg);
            return;
        }

        if (lid >= 0) {
            laik_log(LAIK_LL_Warning, "cannot re-register; already registered with locID %d", lid);
            return;
        }

        char cmd[20], l[50], h[50];
        int p;
        if (sscanf(msg, "%20s %50s %50s %d", cmd, l, h, &p) < 4) {
            laik_log(LAIK_LL_Panic, "cannot parse register command '%s'", msg);
            return;
        }

        lid = ++d->maxid;
        assert(fd >= 0);
        d->fds[fd].lid = lid;
        assert(lid < MAX_PEERS);
        laik_log(1, "TCP2 registered new locID %d: location %s at host %s port %d",
                 lid, l, h, p);

        assert(d->peer[lid].port == -1);
        d->peer[lid].fd = fd;
        d->peer[lid].host = strdup(h);
        d->peer[lid].location = strdup(l);
        d->peer[lid].port = p;
        // first time we use this id for a peer: init receive
        d->peer[lid].rcount = 0;
        d->peer[lid].scount = 0;

        // send location ID info: "id <lid> <location> <host> <port>"
        // new registered id to all already registered peers
        sprintf(msg, "id %d %s %s %d", lid, l, h, p);
        for(int i = 1; i <= d->maxid; i++)
            send_cmd(d, i, msg);
        for(int i = 0; i < d->maxid; i++) {
            sprintf(msg, "id %d %s %s %d", i,
                    d->peer[i].location, d->peer[i].host, d->peer[i].port);
            send_cmd(d, lid, msg);
        }

        d->peers++;
        d->exit = 1;
        return;
    }

    if (msg[0] == 'm') {
        // myid <lid>
        // used on re-connection of other peer: peer must already be known

        char cmd[20];
        int peerid;
        if (sscanf(msg, "%20s %d", cmd, &peerid) < 2) {
            laik_log(LAIK_LL_Panic, "cannot parse myid command '%s'", msg);
            return;
        }

        if (lid >= 0) {
            // if peer already known, id must be same
            if (lid != peerid)
                laik_log(LAIK_LL_Panic, "got ID %d from peer already known with locID %d", peerid, lid);
            return;
        }
        if (d->mylid == peerid) {
            laik_log(LAIK_LL_Panic, "got ID %d from peer which is my own location ID", peerid);
            return;
        }

        lid = peerid;
        assert((lid >= 0) && (lid < MAX_PEERS));
        assert(lid <= d->maxid);
        d->peer[lid].fd = fd;
        assert(fd >= 0);
        d->fds[fd].lid = lid;

        // must already be known, announced by master
        assert(d->peer[lid].location != 0);
        assert(d->peer[lid].host != 0);
        assert(d->peer[lid].port >= 0);

        laik_log(1, "TCP2 seen location ID %d (location %s) at FD %d",
                 lid, d->peer[lid].location, fd);
        return;
    }

    if (msg[0] == 'h') {
        laik_log(1, "TCP2 Sending usage because of help command");

        assert(fd > 0);
        if (lid == -1) lid = -fd;
        send_cmd(d, lid, "# Usage (first char of command is enough):");
        send_cmd(d, lid, "#  data <len> [pos] <hex> ...   : data from a LAIK container");
        send_cmd(d, lid, "#  help                         : this help text");
        send_cmd(d, lid, "#  id <id> <loc> <host> <port>  : announce location id info");
        send_cmd(d, lid, "#  kill                         : ask process to terminate");
        send_cmd(d, lid, "#  myid <id>                    : identify your location id");
        send_cmd(d, lid, "#  phase <phase>                : announce current phase");
        send_cmd(d, lid, "#  quit                         : close connection");
        send_cmd(d, lid, "#  register <loc> <host> <port> : request assignment of id");
        send_cmd(d, lid, "#  status                       : request status output");
        return;
    }

    if (msg[0] == 'k') {
        // kill command - meant for interactive control
        laik_log(1, "TCP2 Exiting because of kill command");

        assert(fd > 0);
        if (lid == -1) lid = -fd;
        send_cmd(d, lid, "# Exiting. Bye");
        exit(1);
    }

    if (msg[0] == 'q') {
        // quit command - meant for interactive control
        laik_log(1, "TCP2 Closing connection because of quit command");

        assert(fd >= 0);
        close(fd);
        rm_rfd(d, fd);
        if (lid >= 0) d->peer[lid].fd = -1;
        return;
    }

    if (msg[0] == '#') {
        // accept but ignore comments: this is for interactive use via nc/telnet
        laik_log(1, "TCP2 Got comment %s", msg);
        return;
    }

    if (msg[0] == 's') {
        // status command
        laik_log(1, "TCP2 Sending status becaue of status command");
        assert(fd > 0);
        if (lid == -1) lid = -fd;
        sprintf(msg, "# My locID is %d", d->mylid);
        send_cmd(d, lid, msg);
        send_cmd(d, lid, "# Processes in world:");
        for(int i = 0; i <= d->maxid; i++) {
            sprintf(msg, "#  LID %2d loc '%s' at %s:%d", i,
                    d->peer[i].location, d->peer[i].host, d->peer[i].port);
            send_cmd(d, lid, msg);
        }
        return;
    }

    // ignore if sender unknown (only register allowed from yet-unknown sender)
    if (lid < 0) {
        laik_log(LAIK_LL_Warning, "ignoring command '%s' from unknown sender", msg);
        assert(fd > 0);
        if (lid == -1) lid = -fd;
        send_cmd(d, lid, "# first register, see 'help'");
        return;
    }

    // second part of commands are accepted only with ID assigned by master

    if (msg[0] == 'i') {
        // id <lid> <location> <host> <port>

        // ignore if master
        if (d->mylid == 0) {
            laik_log(LAIK_LL_Warning, "ignoring id command '%s' as master", msg);
            return;
        }

        char cmd[20], l[50], h[50];
        int p;
        if (sscanf(msg, "%20s %d %50s %50s %d", cmd, &lid, l, h, &p) < 5) {
            laik_log(LAIK_LL_Panic, "cannot parse id command '%s'", msg);
            return;
        }

        assert((lid >= 0) && (lid < MAX_PEERS));
        if (d->mylid < 0) {
            // is this my location id?
            if (strcmp(d->location, l) == 0)
                d->mylid = lid;
        }
        if (d->peer[lid].location != 0) {
            // already known, announced by master
            assert(lid <= d->maxid);
            assert(strcmp(d->peer[lid].location, l) == 0);
            assert(strcmp(d->peer[lid].host, h) == 0);
            assert(d->peer[lid].port == p);
        }
        else {
            d->peer[lid].host = strdup(h);
            d->peer[lid].location = strdup(l);
            d->peer[lid].port = p;
            // first time we see this peer: init receive
            d->peer[lid].rcount = 0;
            d->peer[lid].scount = 0;

            if (lid != d->mylid) d->peers++;
            if (lid > d->maxid) d->maxid = lid;
        }
        laik_log(1, "TCP2 seen %slocID %d (location %s), active peers %d",
                 (lid == d->mylid) ? "my ":"", lid, l,  d->peers);
        return;
    }

    if (msg[0] == 'p') {
        // phase <phaseid>

        // ignore if master
        if (d->mylid == 0) {
            laik_log(LAIK_LL_Warning, "ignoring phase command '%s' as master", msg);
            return;
        }

        char cmd[20];
        int phase;
        if (sscanf(msg, "%20s %d", cmd, &phase) < 2) {
            laik_log(LAIK_LL_Panic, "cannot parse phase command '%s'", msg);
            return;
        }
        laik_log(1, "TCP2 got phase %d", phase);
        d->phase = phase;
        d->exit = 1;
        return;
    }

    if (msg[0] == 'a') {
        // allowsend <count> <elemsize>

        char cmd[20];
        int count, esize;
        if (sscanf(msg, "%20s %d %d", cmd, &count, &esize) < 3) {
            laik_log(LAIK_LL_Panic, "cannot parse allowsend command '%s'", msg);
            return;
        }
        laik_log(1, "TCP2 got allowsend %d %d", count, esize);
        assert(d->peer[lid].scount == 0);
        d->peer[lid].scount = count;
        d->peer[lid].selemsize = esize;
        d->exit = 1;
        return;
    }

    if (msg[0] == 'd') {
        // data <len> [(<pos>)] <hex> ...
        got_data(d, lid, msg);
        return;
    }

    laik_log(LAIK_LL_Warning, "TCP2 got from lID %d unknown msg '%s'", lid, msg);
}

void process_rbuf(InstData* d, int fd)
{
    char* rbuf;
    int used;

    assert((fd >= 0) && (fd < MAX_FDS));
    rbuf = d->fds[fd].rbuf;
    assert(rbuf != 0);
    used = d->fds[fd].rbuf_used;

    laik_log(1, "TCP2 handle commands in receive buf of FD %d (LID %d, %d bytes)\n",
             fd, d->fds[fd].lid, used);

    int pos1 = 0, pos2 = 0;
    while(pos2 < used) {
        if (rbuf[pos2] == 13) {
            // change CR to whitespace (sent by telnet)
            rbuf[pos2] = ' ';
        }
        if (rbuf[pos2] == '\n') {
            rbuf[pos2] = 0;
            got_cmd(d, fd, rbuf + pos1, pos2 - pos1);
            pos1 = pos2+1;
        }
        pos2++;
    }
    if (pos1 > 0) {
        used = 0;
        while(pos1 < pos2)
            rbuf[used++] = rbuf[pos1++];
    }
    d->fds[fd].rbuf_used = used;
}

void got_bytes(InstData* d, int fd)
{
    // use a per-fd receive buffer to not mix partially sent commands
    assert((fd >= 0) && (fd < MAX_FDS));
    int used = d->fds[fd].rbuf_used;

    if (used == RBUF_LEN) {
        // buffer not large enough for even 1 command: should not happen
        laik_panic("TCP2 receive buffer too small for 1 command");
        exit(1);
    }

    char* rbuf = d->fds[fd].rbuf;
    int len = read(fd, rbuf + used, RBUF_LEN - used);
    if (len == -1) {
        int e = errno;
        laik_log(1, "TCP2 warning: read error on FD %d: %s\n",
                 fd, strerror(e));
        return;
    }
    if (len == 0) {
        // other side closed connection

        // process left-over commands, add NL for last command to process
        rbuf[used] = '\n';
        process_rbuf(d, fd);
        assert(d->fds[fd].rbuf_used == 0);

        laik_log(1, "TCP2 FD %d closed (peer LID %d)\n",
                 fd, d->fds[fd].lid);

        close(fd);
        rm_rfd(d, fd);
        return;
    }

    if (laik_log_begin(1)) {
        char lstr[40];
        int i, o;
        o = sprintf(lstr, "%02x", rbuf[used]);
        for(i = 1; (i < len) && (i < 8); i++)
            o += sprintf(lstr + o, " %02x", rbuf[used + i]);
        if (i < len) sprintf(lstr + o, "...");
        laik_log_flush("TCP2 got_bytes(FD %d, peer LID %d, used %d): read %d bytes (%s)\n",
                       fd, d->fds[fd].lid, used, len, lstr);
    }

    d->fds[fd].rbuf_used = used + len;
    process_rbuf(d, fd);
}

void got_connect(InstData* d, int fd)
{
    struct sockaddr saddr;
    socklen_t len = sizeof(saddr);
    int newfd = accept(fd, &saddr, &len);
    if (newfd < 0) {
        laik_panic("TCP2 Error in accept\n");
        exit(1);
    }

    add_rfd(d, newfd, got_bytes);

    char str[20];
    if (saddr.sa_family == AF_INET)
        inet_ntop(AF_INET, &(((struct sockaddr_in*)&saddr)->sin_addr), str, 20);
    if (saddr.sa_family == AF_INET6)
        inet_ntop(AF_INET6, &(((struct sockaddr_in6*)&saddr)->sin6_addr), str, 20);
    laik_log(1, "TCP2 Got connection on FD %d from %s\n", newfd, str);

    send_cmd(d, -newfd, "# Here is LAIK TCP2");
}



//
// backend initialization
//

Laik_Instance* laik_init_tcp2(int* argc, char*** argv)
{
    char* str;

    (void) argc;
    (void) argv;

    if (instance)
        return instance;

    // this is required to not get spurious SIGPIPE signals
    // from opened sockets, e.g. if other side closes connection
    // TODO: LAIK as library should not touch global signal handlers...
    signal(SIGPIPE, SIG_IGN);

    // my location: hostname:PID
    char location[100];
    if (gethostname(location, 100) != 0) {
        // logging not initilized yet
        fprintf(stderr, "TCP2 cannot get host name");
        exit(1);
    }
    char* host = strdup(location);
    sprintf(location + strlen(location), ":%d", getpid());

    // enable early logging
    laik_log_init_loc(location);
    if (laik_log_begin(1)) {
        laik_log_append("TCP2 init: cmdline '%s", (*argv)[0]);
        for(int i = 1; i < *argc; i++)
            laik_log_append(" %s", (*argv)[i]);
        laik_log_flush("'\n");
    }

    // setting of home location: host/port to register with
    str = getenv("LAIK_TCP2_HOST");
    char* home_host = str ? str : "localhost";
    str = getenv("LAIK_TCP2_PORT");
    int home_port = str ? atoi(str) : 0;
    if (home_port == 0) home_port = TCP2_PORT;

    laik_log(1, "TCP2 location %s, home %s:%d\n", location, home_host, home_port);

    InstData* d = malloc(sizeof(InstData) + MAX_PEERS * sizeof(Peer));
    if (!d) {
        laik_panic("TCP2 Out of memory allocating InstData object");
        exit(1); // not actually needed, laik_panic never returns
    }
    d->peers = 0; // zero active peers
    for(int i = 0; i < MAX_PEERS; i++) {
        d->peer[i].port = -1; // unknown peer
        d->peer[i].fd = -1;   // not connected
        d->peer[i].host = 0;
        d->peer[i].location = 0;
        d->peer[i].rcount = 0;
        d->peer[i].scount = 0;
    }

    FD_ZERO(&d->rset);
    d->maxfds = 0;
    d->exit = 0;
    for(int i = 0; i < MAX_FDS; i++)
        d->fds[i].cb = 0;

    d->host = host;
    d->location = strdup(location);
    d->listenfd = -1; // not bound yet
    d->maxid = -1;    // not set yet
    d->phase = -1;    // not set yet
    // if home host is localhost, try to become master (-1: not yet determined)
    d->mylid = check_local(home_host) ? 0 : -1;

    // create socket to listen for incoming TCP connections
    //  if <home_host> is not set, try to aquire local port <home_port>
    // we may need to try creating the listening socket twice
    struct sockaddr_in sin;
    int listenfd = -1;
    while(1) {
        listenfd = socket(PF_INET, SOCK_STREAM, 0);
        if (listenfd < 0) {
            laik_panic("TCP2 cannot create listening socket");
            exit(1); // not actually needed, laik_panic never returns
        }
        if (d->mylid == 0) {
            // mainly for development: avoid wait time to bind to same port
            if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                           &(int){1}, sizeof(int)) < 0) {
                laik_panic("TCP2 cannot set SO_REUSEADDR");
                exit(1); // not actually needed, laik_panic never returns
            }

            sin.sin_family = AF_INET;
            sin.sin_addr.s_addr = htonl(INADDR_ANY);
            sin.sin_port = htons(home_port);
            if (bind(listenfd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
                d->mylid = -1; // already somebody else, still need to determine
            }
            else {
                // listen on successfully bound socket
                // if this fails, another process started listening first
                // and we need to open another socket, as we cannot unbind
                if (listen(listenfd, 5) < 0) {
                    laik_log(1,"listen failed, opening new socket");
                    close(listenfd);
                    continue;
                }
                d->listenport = home_port;
                break;
            }
        }
        // not bound yet: will bind to random port
        if (listen(listenfd, 5) < 0) {
            laik_panic("TCP2 cannot listen on socket");
            exit(1); // not actually needed, laik_panic never returns
        }
        break;
    }

    if (d->mylid < 0) {
        socklen_t len = sizeof(sin);
        if (getsockname(listenfd, (struct sockaddr *)&sin, &len) == -1) {
            laik_panic("TCP2 cannot get port of listening socket");
            exit(1); // not actually needed, laik_panic never returns
        }
        d->listenport = ntohs(sin.sin_port);
    }
    d->listenfd = listenfd;
    laik_log(1, "TCP2 listening on port %d\n", d->listenport);

    // now we know if we are master: init peer with id 0
    d->peer[0].host = (d->mylid == 0) ? host : home_host;
    d->peer[0].port = home_port;
    d->peer[0].location = (d->mylid == 0) ? d->location : 0;

    // notify us on connection requests at listening port
    add_rfd(d, d->listenfd, got_connect);

    // do registration of each non-master with master (using run-loop)
    //  newcomers block until master accepts them
    int world_size = 0; // not detected yet
    if (d->mylid == 0) {
        // master determines world size
        str = getenv("LAIK_SIZE");
        world_size = str ? atoi(str) : 0;
        if (world_size == 0) world_size = 1; // just master alone

        // slot 0 taken by myself
        d->maxid = 0;
        d->phase = 0;

        if (world_size > 1) {
            laik_log(1, "TCP2 master: waiting for %d peers to join\n", world_size - 1);
            // wait for enough peers to register
            while(d->peers + 1 < world_size)
                run_loop(d);

            // send all peers to start at phase 0
            for(int i = 1; i <= d->maxid; i++)
                send_cmd(d, i, "phase 0");
        }
    }
    else {
        // register with master, get world size
        char msg[100];
        sprintf(msg, "register %.30s %.30s %d", location, host, d->listenport);
        send_cmd(d, 0, msg);
        while(d->phase == -1)
            run_loop(d);
        world_size = d->peers + 1;
    }

    instance = laik_new_instance(&laik_backend, world_size, d->mylid, location, d, 0);
    laik_log(2, "TCP2 backend initialized (at '%s', rank %d/%d, listening at %d)\n",
             location, d->mylid, world_size, d->listenport);

    return instance;
}


// helper for exec

// send data with one element of size <s> at pointer <p> to process <lid>
// position <n/idx> added only to allow check at receiver
static
void send_data(int n, int dims, Laik_Index* idx, int toLID, void* p, int s)
{
    char str[100];
    int o = 0;
    o += sprintf(str, "data %d (%d:%s)", s, n, istr(dims, idx));
    for(int i = 0; i < s; i++) {
        int v = ((unsigned char*)p)[i];
        o += sprintf(str+o, " %02x", v);
    }
    if (s == 8) laik_log(1, " pos %s: %f\n", istr(dims, idx), *((double*)p));

    send_cmd((InstData*)instance->backend_data, toLID, str);
}

// send a slice of data from mapping <m> to process <lid>
// if not yet allowed to send data, we have to wait.
// the action sequence ordering makes sure that there must
// be a matching receive action on the receiver side
static
void send_slice(Laik_Mapping* fromMap, Laik_Slice* slc, int toLID)
{
    Laik_Layout* l = fromMap->layout;
    int esize = fromMap->data->elemsize;
    int dims = slc->space->dims;
    assert(fromMap->start != 0); // must be backed by memory

    InstData* d = (InstData*)instance->backend_data;
    Peer* p = &(d->peer[toLID]);
    if (p->scount == 0) {
        // we need to wait for right to send data
        while(p->scount == 0)
            run_loop(d);
    }
    assert(p->scount == (int) laik_slice_size(slc));
    assert(p->selemsize == esize);

    Laik_Index idx = slc->from;
    int ecount = 0;
    while(1) {
        int64_t off = l->offset(l, fromMap->layoutSection, &idx);
        void* idxPtr = fromMap->start + off * esize;
        send_data(ecount, dims, &idx, toLID, idxPtr, esize);
        ecount++;
        if (!next_lex(slc, &idx)) break;
    }
    assert(ecount == (int) laik_slice_size(slc));

    // withdraw our right to send further data
    p->scount = 0;
}

// queue receive action and run event loop until all data received
// <ro> allows to request reduction with existing value
// (use RO_None to overwrite with received value)
static
void recv_slice(Laik_Slice* slc, int fromLID, Laik_Mapping* toMap, Laik_ReductionOperation ro)
{
    assert(toMap->start != 0); // must be backed by memory
    // check no data still registered to be received from <fromID>
    InstData* d = (InstData*)instance->backend_data;
    Peer* p = &(d->peer[fromLID]);
    assert(p->rcount == 0);

    // write outstanding receive info into peer structure
    p->rcount = laik_slice_size(slc);
    assert(p->rcount > 0);
    p->roff = 0;
    p->relemsize = toMap->data->elemsize;
    p->rmap = toMap;
    p->rslc = slc;
    p->ridx = slc->from;
    p->rro = ro;

    // give peer the right to start sending data consisting of given number of elements
    char msg[50];
    sprintf(msg, "allowsend %d %d", p->rcount, p->relemsize);
    send_cmd(d, fromLID, msg);

    // wait until all data received from peer
    while(p->roff < p->rcount)
        run_loop(d);

    // done
    p->rcount = 0;
}

/* reduction at one process using send/recv
 * 
 * One process is chosen to do the reduction (reduceProcess): this is selected
 * to be the process with smallest id of all processes which are interested in the
 * result (input group). All other processes with input send their data to the
 * reduceProcess. It does the reduction, and then it sends the result to all
 * processes interested in the result (output group)
*/
static
void exec_reduce(Laik_TransitionContext* tc,
                 Laik_BackendAction* a)
{
    assert(a->h.type == LAIK_AT_MapGroupReduce);
    Laik_Transition* t = tc->transition;

    // do the manual reduction on smallest rank of output group
    int reduceTask = laik_trans_taskInGroup(t, a->outputGroup, 0);
    int reduceLID = laik_group_locationid(t->group, reduceTask);
    laik_log(1, "  reduce process is T%d (locID %d)", reduceTask, reduceLID);

    int myid = t->group->myid;
    if (myid != reduceTask) {
        // not the reduce process: eventually send input and recv result

        if (laik_trans_isInGroup(t, a->inputGroup, myid)) {
            laik_log(1, "  not reduce process: send to T%d (locID %d)",
                     reduceTask, reduceLID);
            assert(tc->fromList && (a->fromMapNo < tc->fromList->count));
            Laik_Mapping* m = &(tc->fromList->map[a->fromMapNo]);
            send_slice(m, a->slc, reduceLID);
        }
        if (laik_trans_isInGroup(t, a->outputGroup, myid)) {
            laik_log(1, "  not reduce process: recv from T%d (locID%d)",
                     reduceTask, reduceLID);
            assert(tc->toList && (a->toMapNo < tc->toList->count));
            Laik_Mapping* m = &(tc->toList->map[a->toMapNo]);
            recv_slice(a->slc, reduceLID, m, LAIK_RO_None);
        }
        return;
    }

    // this is the reduce process

    assert(tc->toList && (a->toMapNo < tc->toList->count));
    Laik_Mapping* m = &(tc->toList->map[a->toMapNo]);

    // do receive & reduce with all input processes
    Laik_ReductionOperation op = a->redOp;
    if (!laik_trans_isInGroup(t, a->inputGroup, myid)) {
        // no input from me: overwrite my values
        op = LAIK_RO_None;
    }
    int inCount = laik_trans_groupCount(t, a->inputGroup);
    for(int i = 0; i < inCount; i++) {
        int inTask = laik_trans_taskInGroup(t, a->inputGroup, i);
        if (inTask == myid) continue;
        int inLID = laik_group_locationid(t->group, inTask);

        laik_log(1, "  reduce process: recv + %s from T%d (locID %d), count %d",
                 (op == LAIK_RO_None) ? "overwrite":"reduce", inTask, inLID, a->count);
        recv_slice(a->slc, inLID, m, op);
        op = a->redOp; // eventually reset to reduction op from None
    }

    // send result to processes in output group
    int outCount = laik_trans_groupCount(t, a->outputGroup);
    for(int i = 0; i< outCount; i++) {
        int outTask = laik_trans_taskInGroup(t, a->outputGroup, i);
        if (outTask == myid) {
            // that's myself: nothing to do
            continue;
        }
        int outLID = laik_group_locationid(t->group, outTask);

        laik_log(1, "  reduce process: send result to T%d (locID %d)", outTask, outLID);
        send_slice(m, a->slc, outLID);
    }
}


void tcp2_exec(Laik_ActionSeq* as)
{
    if (as->actionCount == 0) {
        laik_log(1, "TCP2 exec: nothing to do\n");
        return;
    }

    if (as->backend == 0) {
        as->backend = &laik_backend;

        // do minimal transformations, sorting send/recv
        laik_log(1, "TCP2 exec: prepare before exec\n");
        laik_log_ActionSeqIfChanged(true, as, "Original sequence");
        bool changed = laik_aseq_splitTransitionExecs(as);
        laik_log_ActionSeqIfChanged(changed, as, "After splitting texecs");
        changed = laik_aseq_sort_2phases(as);
        laik_log_ActionSeqIfChanged(changed, as, "After sorting");

        laik_aseq_calc_stats(as);
        as->backend = 0; // this tells LAIK that no cleanup needed
    }

    Laik_TransitionContext* tc = as->context[0];
    Laik_Action* a = as->action;
    for(unsigned int i = 0; i < as->actionCount; i++, a = nextAction(a)) {
        switch(a->type) {
        case LAIK_AT_MapPackAndSend: {
            Laik_A_MapPackAndSend* aa = (Laik_A_MapPackAndSend*) a;
            int toLID = laik_group_locationid(tc->transition->group, aa->to_rank);
            laik_log(1, "TCP2 MapPackAndSend to T%d (locID %d), %d x %dB\n",
                     aa->to_rank, toLID, aa->count, tc->data->elemsize);
            assert(tc->fromList && (aa->fromMapNo < tc->fromList->count));
            Laik_Mapping* m = &(tc->fromList->map[aa->fromMapNo]);
            send_slice(m, aa->slc, toLID);
            break;
        }
        case LAIK_AT_MapRecvAndUnpack: {
            Laik_A_MapRecvAndUnpack* aa = (Laik_A_MapRecvAndUnpack*) a;
            int fromLID = laik_group_locationid(tc->transition->group, aa->from_rank);
            laik_log(1, "TCP2 MapRecvAndUnpack from T%d (locID %d), %d x %dB\n",
                     aa->from_rank, fromLID, aa->count, tc->data->elemsize);
            assert(tc->toList && (aa->toMapNo < tc->toList->count));
            Laik_Mapping* m = &(tc->toList->map[aa->toMapNo]);
            recv_slice(aa->slc, fromLID, m, LAIK_RO_None);
            break;
        }

        case LAIK_AT_MapGroupReduce: {
            Laik_BackendAction* aa = (Laik_BackendAction*) a;
            laik_log(1, "TCP2 MapGroupReduce %d x %dB\n",
                     aa->count, tc->data->elemsize);
            exec_reduce(tc, aa);
            break;
        }

        default:
            assert(0);
            break;
        }
    }
}

void tcp2_sync(Laik_KVStore* kvs)
{
    // TODO
    (void) kvs;
}

#endif // USE_TCP2
