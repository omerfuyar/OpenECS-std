// The net standard plugin: TCP connections and servers, and UDP sockets, that never block the program (DESIGN 9).
// SDL_net resolves names and moves data in the background; a plugin asks for the state of a connection and takes what arrived when it wants, such as from a timer.

// SDL_net is compiled into this plugin (DESIGN 1); its functions stay inside it, and it finds the network interfaces with getifaddrs, as its own build does on Linux
// its socket code needs the POSIX names that strict C23 leaves out
#define _DEFAULT_SOURCE
#define SDL_DECLSPEC
#define HAVE_GETIFADDRS
#include "src/SDL_net.c"

#include "OpenECS.h"

#pragma region Source Only

/// @brief The most hosts a UDP socket keeps resolved, so that sending to them again does not resolve them again.
#define NET_UDP_HOSTS 8

/// @brief The states of a connection, as net.status gives them.
typedef enum NetState
{
    NetState_Failed = -1, // it could not connect, or it closed
    NetState_Waiting = 0, // the name is being resolved, or the connection being made
    NetState_Connected = 1,
} NetState;

/// @brief A TCP connection: to a host, while its name resolves, then a stream socket.
typedef struct NetConnection
{
    NET_Address *address; // nullptr for a connection that a server accepted
    u16 port;
    NET_StreamSocket *socket; // nullptr until the name resolves, and after it closes
    bool failed;
    u8 *received; // what receive gave last, kept until the next receive
} NetConnection;

/// @brief A datagram waiting for its host's name to resolve.
typedef struct NetPendingDatagram
{
    NET_Address *address;
    u16 port;
    u8 *data;
    usz size;
    struct NetPendingDatagram *next;
} NetPendingDatagram;

/// @brief A UDP socket: datagrams waiting to be sent, the hosts it resolved, and what it received last.
typedef struct NetUdp
{
    NET_DatagramSocket *socket;
    NetPendingDatagram *pending; // in the order they were sent
    struct
    {
        char *host;
        NET_Address *address;
    } hosts[NET_UDP_HOSTS]; // replaced in turn once full
    usz nextHost;
    NET_Datagram *received; // what receiveFrom gave last, kept until the next receiveFrom
} NetUdp;

static struct
{
    ECSPlugin plugin;
    bool started;
} NET = {0};

/// @brief Moves a connection on: makes its socket once its name resolves.
static void NetAdvance(NetConnection *connection)
{
    if (connection->socket != nullptr || connection->failed || connection->address == nullptr)
    {
        return;
    }

    NET_Status status = NET_GetAddressStatus(connection->address);

    if (status == NET_SUCCESS)
    {
        connection->socket = NET_CreateClient(connection->address, connection->port, 0);
        connection->failed = connection->socket == nullptr;
    }
    else if (status == NET_FAILURE)
    {
        connection->failed = true;
    }
}

static NetConnection *NetConnect(const char *host, i32 port)
{
    NetConnection *connection = SDL_calloc(1, sizeof(NetConnection));

    if (connection == nullptr)
    {
        return nullptr;
    }

    connection->port = (u16)SDL_clamp(port, 0, 65535);
    connection->address = NET_ResolveHostname(host);
    connection->failed = connection->address == nullptr;
    return connection;
}

static i32 NetStatus(NetConnection *connection)
{
    NetAdvance(connection);

    if (connection->failed || (connection->socket == nullptr && connection->address == nullptr))
    {
        return NetState_Failed;
    }

    if (connection->socket == nullptr)
    {
        return NetState_Waiting;
    }

    switch (NET_GetConnectionStatus(connection->socket))
    {
    case NET_SUCCESS:
        return NetState_Connected;
    case NET_WAITING:
        return NetState_Waiting;
    default:
        return NetState_Failed;
    }
}

static bool NetSend(NetConnection *connection, SHUSlice data)
{
    NetAdvance(connection);
    return connection->socket != nullptr && data.size <= (usz)SDL_MAX_SINT32 && NET_WriteToStreamSocket(connection->socket, data.data, (int)data.size);
}

static SHUSlice NetReceive(NetConnection *connection, i32 most)
{
    NetAdvance(connection);
    SDL_free(connection->received);
    connection->received = nullptr;

    u8 *buffer = connection->socket == nullptr || most <= 0 ? nullptr : SDL_malloc((usz)most);
    int count = buffer == nullptr ? 0 : NET_ReadFromStreamSocket(connection->socket, buffer, most);

    // a connection that the other side closed gives -1, and is failed from then on
    if (count < 0)
    {
        connection->failed = true;
        count = 0;
    }

    connection->received = buffer;
    return (SHUSlice){.data = buffer, .size = (usz)count};
}

static void NetClose(NetConnection *connection)
{
    NET_DestroyStreamSocket(connection->socket);
    connection->socket = nullptr;
    connection->failed = true;
}

static void NetDestroyConnection(void *object)
{
    NetConnection *connection = object;
    NET_DestroyStreamSocket(connection->socket);
    NET_UnrefAddress(connection->address);
    SDL_free(connection->received);
    SDL_free(connection);
}

static NET_Server *NetListen(i32 port)
{
    u16 number = (u16)SDL_clamp(port, 0, 65535);
    NET_Server *server = NET_CreateServer(nullptr, number, 0);

    // SDL_net listens on every family of addresses at once, and fails if one is missing, such as IPv6; then IPv4 alone is tried
    if (server == nullptr)
    {
        NET_Address *any = NET_ResolveHostname("0.0.0.0");

        if (any != nullptr && NET_WaitUntilResolved(any, 1000) == NET_SUCCESS)
        {
            server = NET_CreateServer(any, number, 0);
        }

        NET_UnrefAddress(any);
    }

    if (server == nullptr)
    {
        ECS_LogWarn(NET.plugin, "Cannot listen on port %d: %s", port, SDL_GetError());
    }

    return server;
}

static NetConnection *NetAccept(NET_Server *server)
{
    NET_StreamSocket *socket = nullptr;

    if (!NET_AcceptClient(server, &socket) || socket == nullptr)
    {
        return nullptr;
    }

    NetConnection *connection = SDL_calloc(1, sizeof(NetConnection));

    if (connection == nullptr)
    {
        NET_DestroyStreamSocket(socket);
        return nullptr;
    }

    connection->socket = socket;
    return connection;
}

static void NetDestroyServer(void *object)
{
    NET_DestroyServer(object);
}

static NetUdp *NetUdpOpen(i32 port)
{
    NetUdp *udp = SDL_calloc(1, sizeof(NetUdp));

    if (udp == nullptr)
    {
        return nullptr;
    }

    u16 number = (u16)SDL_clamp(port, 0, 65535);
    udp->socket = NET_CreateDatagramSocket(nullptr, number, 0);

    // as for a server, IPv4 alone is tried when a family of addresses is missing
    if (udp->socket == nullptr)
    {
        NET_Address *any = NET_ResolveHostname("0.0.0.0");

        if (any != nullptr && NET_WaitUntilResolved(any, 1000) == NET_SUCCESS)
        {
            udp->socket = NET_CreateDatagramSocket(any, number, 0);
        }

        NET_UnrefAddress(any);
    }

    if (udp->socket == nullptr)
    {
        ECS_LogWarn(NET.plugin, "Cannot open a UDP socket on port %d: %s", port, SDL_GetError());
        SDL_free(udp);
        return nullptr;
    }

    return udp;
}

/// @brief Sends the waiting datagrams whose hosts resolved, in order; one whose host cannot be resolved is dropped.
static void NetUdpFlush(NetUdp *udp)
{
    while (udp->pending != nullptr)
    {
        NetPendingDatagram *datagram = udp->pending;
        NET_Status status = NET_GetAddressStatus(datagram->address);

        if (status == NET_WAITING)
        {
            return;
        }

        if (status == NET_SUCCESS)
        {
            NET_SendDatagram(udp->socket, datagram->address, datagram->port, datagram->data, (int)datagram->size);
        }

        udp->pending = datagram->next;
        NET_UnrefAddress(datagram->address);
        SDL_free(datagram->data);
        SDL_free(datagram);
    }
}

/// @brief Gives a host's address, resolving it once per socket.
/// @return A reference the caller owns, or nullptr if the name cannot be resolved.
static NET_Address *NetUdpAddress(NetUdp *udp, const char *host)
{
    for (usz i = 0; i < NET_UDP_HOSTS; i++)
    {
        if (udp->hosts[i].host != nullptr && SDL_strcmp(udp->hosts[i].host, host) == 0)
        {
            return NET_RefAddress(udp->hosts[i].address);
        }
    }

    NET_Address *address = NET_ResolveHostname(host);
    char *copy = address == nullptr ? nullptr : SDL_strdup(host);

    if (copy != nullptr)
    {
        usz slot = udp->nextHost;
        udp->nextHost = (udp->nextHost + 1) % NET_UDP_HOSTS;
        SDL_free(udp->hosts[slot].host);
        NET_UnrefAddress(udp->hosts[slot].address);
        udp->hosts[slot].host = copy;
        udp->hosts[slot].address = NET_RefAddress(address);
    }

    return address;
}

static bool NetUdpSendTo(NetUdp *udp, const char *host, i32 port, SHUSlice data)
{
    NetPendingDatagram *datagram = data.size <= (usz)SDL_MAX_SINT32 ? SDL_calloc(1, sizeof(NetPendingDatagram)) : nullptr;
    datagram = datagram == nullptr ? nullptr : (datagram->data = SDL_malloc(SDL_max(data.size, 1)), datagram);

    if (datagram == nullptr || datagram->data == nullptr || (datagram->address = NetUdpAddress(udp, host)) == nullptr)
    {
        SDL_free(datagram == nullptr ? nullptr : datagram->data);
        SDL_free(datagram);
        return false;
    }

    SDL_memcpy(datagram->data, data.data, data.size);
    datagram->size = data.size;
    datagram->port = (u16)SDL_clamp(port, 0, 65535);

    // it waits behind the others, so that datagrams to a host leave in the order they were sent
    NetPendingDatagram **last = &udp->pending;

    while (*last != nullptr)
    {
        last = &(*last)->next;
    }

    *last = datagram;
    NetUdpFlush(udp);
    return true;
}

static SHUSlice NetUdpReceiveFrom(NetUdp *udp, const char **retHost, i32 *retPort)
{
    NetUdpFlush(udp);
    NET_DestroyDatagram(udp->received);
    udp->received = nullptr;
    *retHost = nullptr;
    *retPort = 0;

    if (!NET_ReceiveDatagram(udp->socket, &udp->received) || udp->received == nullptr)
    {
        return (SHUSlice){0};
    }

    *retHost = NET_GetAddressString(udp->received->addr);
    *retPort = udp->received->port;
    return (SHUSlice){.data = udp->received->buf, .size = (usz)udp->received->buflen};
}

static void NetDestroyUdp(void *object)
{
    NetUdp *udp = object;

    while (udp->pending != nullptr)
    {
        NetPendingDatagram *next = udp->pending->next;
        NET_UnrefAddress(udp->pending->address);
        SDL_free(udp->pending->data);
        SDL_free(udp->pending);
        udp->pending = next;
    }

    for (usz i = 0; i < NET_UDP_HOSTS; i++)
    {
        SDL_free(udp->hosts[i].host);
        NET_UnrefAddress(udp->hosts[i].address);
    }

    NET_DestroyDatagram(udp->received);
    NET_DestroyDatagramSocket(udp->socket);
    SDL_free(udp);
}

static void NetFree(void *data)
{
    (void)data;

    if (NET.started)
    {
        NET_Quit();
    }

    SDL_zero(NET);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    NET.plugin = plugin;
    NET.started = NET_Init();

    if (!NET.started)
    {
        ECS_LogError(plugin, "Cannot start SDL_net: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "connection", NetDestroyConnection), NetFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "server", NetDestroyServer), NetFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "udp", NetDestroyUdp), NetFree(nullptr););

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"connect", (ECSFunction)NetConnect, "handle<net.connection>(string host, int port)", "Start a TCP connection to a host and port"},
        {"status", (ECSFunction)NetStatus, "int(handle<net.connection> connection)", "Give a connection's state: 1 connected, 0 waiting, -1 failed or closed"},
        {"send", (ECSFunction)NetSend, "bool(handle<net.connection> connection, buffer data)", "Send data; it goes out in the background"},
        {"receive", (ECSFunction)NetReceive, "buffer(handle<net.connection> connection, int most)", "Take what has arrived, at most a number of bytes; nothing if nothing has"},
        {"close", (ECSFunction)NetClose, "void(handle<net.connection> connection)", "Close a connection"},
        {"listen", (ECSFunction)NetListen, "handle<net.server>(int port)", "Wait for TCP connections on a port of every address of this computer"},
        {"accept", (ECSFunction)NetAccept, "handle<net.connection>(handle<net.server> server)", "Take a connection that a server has, if any"},
        {"udp", (ECSFunction)NetUdpOpen, "handle<net.udp>(int port)", "Open a UDP socket on a port of every address of this computer; port 0 picks a free one"},
        {"sendTo", (ECSFunction)NetUdpSendTo, "bool(handle<net.udp> socket, string host, int port, buffer data)", "Send a datagram to a host and port; it leaves once the host's name resolves, and may be lost on the way"},
        {"receiveFrom", (ECSFunction)NetUdpReceiveFrom, "buffer(handle<net.udp> socket, out string host, out int port)", "Take a datagram that has arrived, with the address and port it came from; nothing if none has"},
    };

    // a plugin whose Init fails is not destroyed, so it cleans up here
    const ECSPluginDesc desc = {.Destroy = NetFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc), NetFree(nullptr););

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), NetFree(nullptr););
    }

    return SHUResult_Ok;
}
