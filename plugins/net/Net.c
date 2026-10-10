// The net standard plugin: TCP connections and servers that never block the program (DESIGN 7).
// SDL_net resolves names and moves data in the background; a plugin asks for the state of a connection and takes what arrived when it wants, such as from a timer.

// SDL_net is compiled into this plugin (DESIGN 1); its functions stay inside it, and it finds the network interfaces with getifaddrs, as its own build does on Linux
// its socket code needs the POSIX names that strict C23 leaves out
#define _DEFAULT_SOURCE
#define SDL_DECLSPEC
#define HAVE_GETIFADDRS
#include "src/SDL_net.c"

#include "OpenECS.h"

#pragma region Source Only

/// @brief A name of the plugin: "net." and a local name.
#define NET_NAME(localName) "net." localName

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
    NET_Address *address; // NULL for a connection that a server accepted
    u16 port;
    NET_StreamSocket *socket; // NULL until the name resolves, and after it closes
    bool failed;
    u8 *received; // what receive gave last, kept until the next receive
} NetConnection;

static struct
{
    ECSPlugin plugin;
    bool started;
} NET = {0};

/// @brief Moves a connection on: makes its socket once its name resolves.
static void NetAdvance(NetConnection *connection)
{
    if (connection->socket != NULL || connection->failed || connection->address == NULL)
    {
        return;
    }

    NET_Status status = NET_GetAddressStatus(connection->address);

    if (status == NET_SUCCESS)
    {
        connection->socket = NET_CreateClient(connection->address, connection->port, 0);
        connection->failed = connection->socket == NULL;
    }
    else if (status == NET_FAILURE)
    {
        connection->failed = true;
    }
}

static NetConnection *NetConnect(const char *host, i32 port)
{
    NetConnection *connection = SDL_calloc(1, sizeof(NetConnection));

    if (connection == NULL)
    {
        return NULL;
    }

    connection->port = (u16)SDL_clamp(port, 0, 65535);
    connection->address = NET_ResolveHostname(host);
    connection->failed = connection->address == NULL;
    return connection;
}

static i32 NetStatus(NetConnection *connection)
{
    NetAdvance(connection);

    if (connection->failed || (connection->socket == NULL && connection->address == NULL))
    {
        return NetState_Failed;
    }

    if (connection->socket == NULL)
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
    return connection->socket != NULL && data.size <= (usz)SDL_MAX_SINT32 && NET_WriteToStreamSocket(connection->socket, data.data, (int)data.size);
}

static SHUSlice NetReceive(NetConnection *connection, i32 most)
{
    NetAdvance(connection);
    SDL_free(connection->received);
    connection->received = NULL;

    u8 *buffer = connection->socket == NULL || most <= 0 ? NULL : SDL_malloc((usz)most);
    int count = buffer == NULL ? 0 : NET_ReadFromStreamSocket(connection->socket, buffer, most);

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
    connection->socket = NULL;
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
    NET_Server *server = NET_CreateServer(NULL, number, 0);

    // SDL_net listens on every family of addresses at once, and fails if one is missing, such as IPv6; then IPv4 alone is tried
    if (server == NULL)
    {
        NET_Address *any = NET_ResolveHostname("0.0.0.0");

        if (any != NULL && NET_WaitUntilResolved(any, 1000) == NET_SUCCESS)
        {
            server = NET_CreateServer(any, number, 0);
        }

        NET_UnrefAddress(any);
    }

    if (server == NULL)
    {
        ECS_Log(NET.plugin, ECSLogLevel_Warning, "Cannot listen on port %d: %s", port, SDL_GetError());
    }

    return server;
}

static NetConnection *NetAccept(NET_Server *server)
{
    NET_StreamSocket *socket = NULL;

    if (!NET_AcceptClient(server, &socket) || socket == NULL)
    {
        return NULL;
    }

    NetConnection *connection = SDL_calloc(1, sizeof(NetConnection));

    if (connection == NULL)
    {
        NET_DestroyStreamSocket(socket);
        return NULL;
    }

    connection->socket = socket;
    return connection;
}

static void NetDestroyServer(void *object)
{
    NET_DestroyServer(object);
}

static void NetFree(void)
{
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
        ECS_Log(plugin, ECSLogLevel_Error, "Cannot start SDL_net: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    SHU_ReturnResult(ECSHandle_RegisterType(plugin, NET_NAME("connection"), NetDestroyConnection), NetFree(););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, NET_NAME("server"), NetDestroyServer), NetFree(););

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {NET_NAME("connect"), (ECSFunction)NetConnect, "handle<net.connection>(string host, int port)", "Start a TCP connection to a host and port"},
        {NET_NAME("status"), (ECSFunction)NetStatus, "int(handle<net.connection> connection)", "Give a connection's state: 1 connected, 0 waiting, -1 failed or closed"},
        {NET_NAME("send"), (ECSFunction)NetSend, "bool(handle<net.connection> connection, buffer data)", "Send data; it goes out in the background"},
        {NET_NAME("receive"), (ECSFunction)NetReceive, "buffer(handle<net.connection> connection, int most)", "Take what has arrived, at most a number of bytes; nothing if nothing has"},
        {NET_NAME("close"), (ECSFunction)NetClose, "void(handle<net.connection> connection)", "Close a connection"},
        {NET_NAME("listen"), (ECSFunction)NetListen, "handle<net.server>(int port)", "Wait for TCP connections on a port of every address of this computer"},
        {NET_NAME("accept"), (ECSFunction)NetAccept, "handle<net.connection>(handle<net.server> server)", "Take a connection that a server has, if any"},
    };

    // a plugin whose Init fails gets no Shutdown, so it cleans up here
    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), NetFree(););
    }

    return SHUResult_Ok;
}

void ECSPlugin_Shutdown(ECSPlugin plugin)
{
    (void)plugin;
    NetFree();
}
