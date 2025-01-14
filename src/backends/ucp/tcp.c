//*********************************************************************************
#include "tcp.h"

#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

//*********************************************************************************
static int socket_fd;
// only used in main process
static int *fds;

//*********************************************************************************
// forward declaration
bool check_local(char *host);

//*********************************************************************************
static inline void send_instance_data(InstData *d, int fd, int lid)
{
    write(fd, &lid, sizeof(int));
    write(fd, &d->world_size, sizeof(int));
    write(fd, &d->phase, sizeof(int));
    write(fd, &d->epoch, sizeof(int));

    for (int k = 0; k < d->world_size; k++)
    {
        write(fd, &(d->peer[k].addrlen), sizeof(size_t));
        write(fd, d->peer[k].address, d->peer[k].addrlen);
    }
}

//*********************************************************************************
static inline void receive_instance_data(InstData *d, int fd)
{
    read(fd, &(d->mylid), sizeof(int));
    read(fd, &(d->world_size), sizeof(int));
    read(fd, &d->phase, sizeof(int));
    read(fd, &d->epoch, sizeof(int));

    d->peer = (Peer *)malloc(d->world_size * sizeof(Peer));
    if (d->peer == NULL)
    {
        laik_panic("Could not malloc heap for non master\n");
    }

    for (int i = 0; i < d->world_size; i++)
    {
        read(fd, &d->peer[i].addrlen, sizeof(d->peer[i].addrlen));
        d->peer[i].address = (ucp_address_t *)malloc(d->peer[i].addrlen);
        if (d->peer[i].address == NULL)
        {
            laik_panic("Could not allocate heap for peer address\n");
        }
        read(fd, d->peer[i].address, d->peer[i].addrlen);
    }
}

//*********************************************************************************
void tcp_initialize_setup_connection(char *home_host, const int home_port, InstData *d)
{ //
    // create listening socket and determine who is master
    //

    // create socket to listen for incoming TCP connections
    //  if <home_host> is not set, try to aquire local port <home_port>
    // we may need to try creating the listening socket twice
    struct sockaddr_in sin;
    socket_fd = -1;
    // if home host is localhost, try to become master
    bool try_master = check_local(home_host);
    struct addrinfo sock_hints = {0}, *res;
    // get address of home node
    sock_hints.ai_family = AF_INET;
    sock_hints.ai_socktype = SOCK_STREAM;
    getaddrinfo(home_host, HOME_PORT_STR, &sock_hints, &res);

    socket_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
    {
        laik_panic("UCP cannot create listening socket");
    }

    if (try_master)
    {
        // mainly for development: avoid wait time to bind to same port
        if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR,
                       &(int){1}, sizeof(int)) < 0)
        {
            laik_panic("UCP cannot set SO_REUSEADDR");
        }

        sin.sin_family = AF_INET;
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
        sin.sin_port = htons(home_port);
        // bind() is a race condition, listening after the bind will only work on one process though
        if (bind(socket_fd, (struct sockaddr *)&sin, sizeof(sin)) == 0)
        {
            // listen on successfully bound socket
            // if this fails, another process started listening first
            // and we need to open another socket, as we cannot unbind
            /// TODO: how many processes should be queued?
            if (listen(socket_fd, 500) < 0)
            {
                close(socket_fd);
                laik_log(1, "Another process is already master, opening new socket\n");
            }
            else
            {
                // we successfully became master: my LID is 0
                d->mylid = 0;
            }
        }
    }

    // now we know if we are master: init peer with id 0
    bool is_master = (d->mylid == 0);

    if (is_master)
    {
        laik_log(1, "I am master!\n");

        // add LID tag to my location
        // copy my data also to d->peer[0]
        d->peer = (Peer *)calloc(d->world_size, sizeof(Peer));
        if (d->peer == NULL)
        {
            laik_panic("Could not malloc heap for peers\n");
        }

        d->peer[0].address = d->address;
        d->peer[0].addrlen = d->addrlen;

        fds = (int *)calloc(d->world_size, sizeof(int));
        if (fds == NULL)
        {
            laik_panic("Could not malloc heap for fds\n");
        }
        for (int i = 1; i < d->world_size; i++)
        {
            fds[i] = accept(socket_fd, NULL, NULL);
            laik_log(1, "%d out of %d is connecting...\n", i, d->world_size - 1);
            if (fds[i] < 0)
                laik_log(LAIK_LL_Panic, "Failed to accept connection: %s\n",
                         strerror(errno));

            // the length of the ucx worker addresses does not have to be the same across the nodes
            laik_log(1, "Master accepted initial Rank [%d]\n", i);
            read(fds[i], &d->peer[i].addrlen, sizeof(d->peer[i].addrlen));

            d->peer[i].address = (ucp_address_t *)malloc(d->peer[i].addrlen);
            if (d->peer[i].address == NULL)
            {
                laik_panic("Could not allocate heap for peer address\n");
            }
            read(fds[i], d->peer[i].address, d->peer[i].addrlen);
        }
        // send assigned number and address list to every non-master node
        for (int i = 1; i < d->world_size; i++)
        {
            send_instance_data(d, fds[i], i);
        }
    }
    else
    {
        socket_fd = socket(PF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0)
        {
            laik_panic("UCP cannot create listening socket");
        }

        if (connect(socket_fd, res->ai_addr, res->ai_addrlen) != 0)
        {
            laik_log(LAIK_LL_Error, "Could not connect to socket: %s\n", strerror(errno));
            exit(1);
        }

        write(socket_fd, &d->addrlen, sizeof(size_t));
        write(socket_fd, d->address, d->addrlen);

        receive_instance_data(d, socket_fd);

        if (d->mylid < 0)
        {
            laik_log(LAIK_LL_Error, "In non master happened something bad id: %d world size %d phase %d and epoch %d\n Last state of errno %s",
                     d->mylid, d->world_size, d->phase, d->epoch, strerror(errno));
        }
    }
}

//*********************************************************************************
size_t tcp_initialize_new_peers(InstData *d)
{
    int old_world_size = d->world_size;
    read(socket_fd, &(d->world_size), sizeof(int));

    laik_log(1, "Rank [%d] received new world size [%d] during init, old world size is [%d]. Product is %lu\n", d->mylid, d->world_size, old_world_size, d->world_size * sizeof(Peer));

    d->peer = (Peer *)realloc(d->peer, d->world_size * sizeof(Peer));

    if (d->peer == NULL)
    {
        laik_panic("Not enough memory for peers\n");
    }

    for (int i = old_world_size; i < d->world_size; i++)
    {
        read(socket_fd, &(d->peer[i].addrlen), sizeof(size_t));
        d->peer[i].address = (ucp_address_t *)malloc(d->peer[i].addrlen);
        if (d->peer[i].address == NULL)
        {
            laik_panic("Not enough memory for peer address\n");
        }
        read(socket_fd, d->peer[i].address, d->peer[i].addrlen);
    }

    return d->world_size - old_world_size;
}

//*********************************************************************************
size_t add_new_peers_master(InstData *d, Laik_Instance *instance)
{
    struct pollfd pfd = {
        .fd = socket_fd,
        .events = POLLIN,
        .revents = 0};

    int old_world_size = d->world_size;

    int result = -1;
    while (((result = poll(&pfd, 1, 0)) > 0) && (pfd.revents == POLLIN))
    {
        fds = realloc(fds, (d->world_size + 1) * sizeof(int));
        if (fds == NULL)
        {
            laik_panic("Not enough memory for fds\n");
        }
        fds[d->world_size] = accept(socket_fd, NULL, NULL);

        if (fds[d->world_size] < 0)
        {
            laik_log(LAIK_LL_Error, "Server could not accept new connection. %s\n", strerror(errno));
        }

        d->world_size++;

        laik_log(1, "Master accepted new connection. World size increased to %d\n", d->world_size);
    }
    if (result < 0)
    {
        laik_panic("Master encountered error while polling new connections\n");
    }

    int number_new_connections = d->world_size - old_world_size;
    assert(number_new_connections >= 0);

    for (int i = 1; i < old_world_size; i++)
    {
        write(fds[i], &number_new_connections, sizeof(int));
    }

    if (number_new_connections == 0)
    {
        laik_log(1, "Nothing has to be done in resize!\n");
        return 0;
    }

    d->peer = (Peer *)realloc(d->peer, d->world_size * sizeof(Peer));
    if (d->peer == NULL)
    {
        laik_panic("Not enough memory for peers\n");
    }

    for (int i = old_world_size; i < d->world_size; i++)
    {
        read(fds[i], &(d->peer[i].addrlen), sizeof(size_t));
        d->peer[i].address = (ucp_address_t *)malloc(d->peer[i].addrlen);

        if (d->peer[i].address == NULL)
        {
            laik_panic("Not enough memory for peer address\n");
        }

        read(fds[i], d->peer[i].address, d->peer[i].addrlen);

        laik_log(1, "Received new address with length %lu!\n", d->peer[i].addrlen);
    }

    int epoch = laik_epoch(instance);
    int phase = laik_phase(instance);

    for (int i = old_world_size; i < d->world_size; i++)
    {
        // broadcast inst data to newcomers
        laik_log(1, "Sending information to newcomer Rank [%d]\n", i);
        write(fds[i], &i, sizeof(int));
        write(fds[i], &(old_world_size), sizeof(int));
        write(fds[i], &phase, sizeof(int));
        write(fds[i], &epoch, sizeof(int));

        // broadcast old rank addresses to newcomers
        for (int k = 0; k < old_world_size; k++)
        {
            write(fds[i], &(d->peer[k].addrlen), sizeof(size_t));
            write(fds[i], d->peer[k].address, d->peer[k].addrlen);
        }

        // broadcast new world size and newcomer addresses to newcomers
        write(fds[i], &(d->world_size), sizeof(int));
        for (int k = old_world_size; k < d->world_size; k++)
        {
            write(fds[i], &(d->peer[k].addrlen), sizeof(size_t));
            write(fds[i], d->peer[k].address, d->peer[k].addrlen);
        }
    }

    for (int i = 1; i < old_world_size; i++)
    {
        for (int k = old_world_size; k < d->world_size; k++)
        {
            write(fds[i], &(d->peer[k].addrlen), sizeof(size_t));
            write(fds[i], d->peer[k].address, d->peer[k].addrlen);
        }
    }

    return number_new_connections;
}

//*********************************************************************************
size_t add_new_peers_non_master(InstData *d, Laik_Instance *instance)
{
    (void)instance;

    int number_new_connections = 0;
    read(socket_fd, &number_new_connections, sizeof(int));

    laik_log(1, "Rank [%d] received %d new connections\n", d->mylid, number_new_connections);

    if (number_new_connections > 0)
    {
        int old_world_size = d->world_size;
        d->world_size = old_world_size + number_new_connections;
        laik_log(1, "Rank [%d] received new world size [%d] from master\n", d->mylid, d->world_size);
        d->peer = (Peer *)realloc(d->peer, (d->world_size) * sizeof(Peer));
        if (d->peer == NULL)
        {
            laik_panic("Not enough memory for peers\n");
        }

        for (int i = old_world_size; i < d->world_size; i++)
        {
            read(socket_fd, &(d->peer[i].addrlen), sizeof(size_t));
            d->peer[i].address = (ucp_address_t *)malloc(d->peer[i].addrlen);
            if (d->peer[i].address == NULL)
            {
                laik_panic("Not enough memory for peer address\n");
            }
            read(socket_fd, d->peer[i].address, d->peer[i].addrlen);
        }
    }

    return number_new_connections;
}

//*********************************************************************************
size_t tcp_add_new_peers(InstData *d, Laik_Instance *instance)
{
    size_t number_new_connections;
    if (d->mylid == 0)
    {
        number_new_connections = add_new_peers_master(d, instance);
    }
    else
    {
        number_new_connections = add_new_peers_non_master(d, instance);
    }
    return number_new_connections;
}

//*********************************************************************************
size_t tcp_remove_peer(InstData *d, Laik_Instance *instance)
{
    (void)d;
    (void)instance;

    return 0;
}

//*********************************************************************************