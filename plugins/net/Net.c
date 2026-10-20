// The net standard plugin: TCP connections and servers, TLS connections, and UDP sockets, that never block the program (DESIGN 9).
// SDL_net resolves names and moves data in the background; a plugin asks for the state of a connection and takes what arrived when it wants, such as from a timer.

// SDL_net is compiled into this plugin (DESIGN 1); its functions stay inside it, and it finds the network interfaces with getifaddrs, as its own build does on Linux
// its socket code needs the POSIX names that strict C23 leaves out
#define _DEFAULT_SOURCE
#define SDL_DECLSPEC
#define HAVE_GETIFADDRS
// on Windows, SDL_net names its socket functions read and write, which MinGW's io.h declares too, so they get names of their own here;
// io.h comes first, so its declarations keep theirs
#ifdef _WIN32
#include <io.h>
#define read NetSocketRead
#define write NetSocketWrite
#endif
#include "src/SDL_net.c"
#ifdef _WIN32
#undef read
#undef write
#endif

#include "OpenECS.h"

// TLS comes from mbedTLS, which is compiled into this plugin too
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/platform.h>
#include <mbedtls/ssl.h>
#include <psa/crypto.h>

// Windows keeps its trusted certificates in a store of its own, which crypt32 reads
#if defined(SDL_PLATFORM_WINDOWS)
#include <windows.h>
#include <wincrypt.h>
#endif

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
    mbedtls_ssl_context *tls; // a TLS connection's state, or nullptr for plain TCP
    bool secured;             // the TLS handshake is done
    struct NetServer *from;   // the TLS server that accepted it, whose configuration it uses, or nullptr
} NetConnection;

/// @brief A server that waits for TCP connections, and for TLS, its certificate, key and configuration.
typedef struct NetServer
{
    NET_Server *server;      // nullptr once its handle is gone
    mbedtls_ssl_config *tls; // nullptr for plain TCP
    mbedtls_x509_crt certificate;
    mbedtls_pk_context key;
    u32 users; // its handle, and each TLS connection it accepted, as they use its configuration
} NetServer;

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
typedef struct NetUDP
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
} NetUDP;

static struct
{
    ECSPlugin plugin;
    bool started;

    // what every TLS connection shares, made at the first one
    bool tlsReady;
    mbedtls_ssl_config tlsConfig;
    mbedtls_x509_crt certificates; // the system's trusted certificates
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
} NET = {0};

/// @brief Gives the system's file of trusted certificates, which TLS checks servers with: SSL_CERT_FILE, or the file of a known distribution.
static const char *NetFindCertificates(void)
{
    const char *const paths[] = {
        SDL_getenv("SSL_CERT_FILE"),
        "/etc/ssl/certs/ca-certificates.crt", // Debian, Ubuntu, Arch
        "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",             // openSUSE
        "/etc/ssl/cert.pem",                  // Alpine
    };

    for (usz i = 0; i < (usz)SDL_arraysize(paths); i++)
    {
        if (paths[i] != nullptr && SDL_GetPathInfo(paths[i], nullptr))
        {
            return paths[i];
        }
    }

    return nullptr;
}

#if defined(SDL_PLATFORM_WINDOWS)
/// @brief Adds the certificates of Windows' store of trusted root certificates to the trusted ones. Gives how many it added.
static usz NetReadWindowsCertificates(void)
{
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    usz count = 0;

    if (store == nullptr)
    {
        return 0;
    }

    // each call frees the certificate it is given, and the last one gives nullptr
    for (PCCERT_CONTEXT certificate = CertEnumCertificatesInStore(store, nullptr); certificate != nullptr; certificate = CertEnumCertificatesInStore(store, certificate))
    {
        if ((certificate->dwCertEncodingType & X509_ASN_ENCODING) != 0 &&
            mbedtls_x509_crt_parse_der(&NET.certificates, certificate->pbCertEncoded, certificate->cbCertEncoded) == 0)
        {
            count++;
        }
    }

    CertCloseStore(store, 0);
    return count;
}
#endif

/// @brief Makes what TLS connections share, once: the random generator, the trusted certificates and the configuration of a client.
static void NetTLSStop(void);

static bool NetTLSStart(void)
{
    if (NET.tlsReady)
    {
        return true;
    }

    mbedtls_ssl_config_init(&NET.tlsConfig);
    mbedtls_x509_crt_init(&NET.certificates);
    mbedtls_entropy_init(&NET.entropy);
    mbedtls_ctr_drbg_init(&NET.random);
    NET.tlsReady = true;

    // a file of trusted certificates comes first; Windows has none, so its store is read when SSL_CERT_FILE names none
    const char *file = NetFindCertificates();
    bool trusted = file != nullptr && mbedtls_x509_crt_parse_file(&NET.certificates, file) >= 0;

#if defined(SDL_PLATFORM_WINDOWS)
    trusted = trusted || NetReadWindowsCertificates() > 0;
#endif

    if (!trusted)
    {
        ECS_LogWarn(NET.plugin, "No trusted certificates can be read, so TLS connections fail; SSL_CERT_FILE can name a file of them");
    }

    if (psa_crypto_init() != PSA_SUCCESS ||
        mbedtls_ctr_drbg_seed(&NET.random, mbedtls_entropy_func, &NET.entropy, (const unsigned char *)"openecs.net", 11) != 0 ||
        mbedtls_ssl_config_defaults(&NET.tlsConfig, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
    {
        // a later connection tries again from the start
        ECS_LogError(NET.plugin, "Cannot start TLS");
        NetTLSStop();
        return false;
    }

    mbedtls_ssl_conf_authmode(&NET.tlsConfig, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&NET.tlsConfig, &NET.certificates, nullptr);
    mbedtls_ssl_conf_rng(&NET.tlsConfig, mbedtls_ctr_drbg_random, &NET.random);
    return true;
}

static void NetTLSStop(void)
{
    if (NET.tlsReady)
    {
        mbedtls_ssl_config_free(&NET.tlsConfig);
        mbedtls_x509_crt_free(&NET.certificates);
        mbedtls_ctr_drbg_free(&NET.random);
        mbedtls_entropy_free(&NET.entropy);
        mbedtls_psa_crypto_free();
        NET.tlsReady = false;
    }
}

/// @brief A full path: a relative path starts at the executable's folder, as the paths of the other standard plugins do. Free it with SDL_free.
static char *NetFull(const char *path)
{
    char *full = nullptr;
    return SDL_asprintf(&full, "%s%s", ECSPath_IsAbsolute(path) ? "" : SDL_GetBasePath(), path) < 0 ? nullptr : full;
}

/// @brief Adds the certificates of a PEM file to the trusted ones, which later TLS connections check servers with.
static bool NetTrust(const char *path)
{
    char *full = NetFull(path);

    // as with the system's file, the certificates that parse are trusted, and those that do not are reported
    int failed = full != nullptr && NetTLSStart() ? mbedtls_x509_crt_parse_file(&NET.certificates, full) : -1;
    bool trusted = failed >= 0;

    if (!trusted)
    {
        ECS_LogWarn(NET.plugin, "Cannot read the certificates of '%s'", full != nullptr ? full : path);
    }
    else if (failed > 0)
    {
        ECS_LogWarn(NET.plugin, "%d certificates of '%s' cannot be read; the others are trusted", failed, full);
    }

    SDL_free(full);
    return trusted;
}

/// @brief Lets go of a server, and frees it once nothing uses it.
static void NetReleaseServer(NetServer *server)
{
    if (--server->users > 0)
    {
        return;
    }

    if (server->tls != nullptr)
    {
        mbedtls_ssl_config_free(server->tls);
        SDL_free(server->tls);
    }

    mbedtls_x509_crt_free(&server->certificate);
    mbedtls_pk_free(&server->key);
    NET_DestroyServer(server->server);
    SDL_free(server);
}

/// @brief Gives TLS's bytes to the socket; SDL_net queues them all.
static int NetTLSSend(void *data, const unsigned char *bytes, size_t length)
{
    NetConnection *connection = data;
    return length <= (usz)SDL_MAX_SINT32 && NET_WriteToStreamSocket(connection->socket, bytes, (int)length) ? (int)length : MBEDTLS_ERR_NET_SEND_FAILED;
}

/// @brief Takes the bytes that have arrived for TLS; none yet means TLS waits.
static int NetTLSReceive(void *data, unsigned char *bytes, size_t length)
{
    NetConnection *connection = data;
    int count = NET_ReadFromStreamSocket(connection->socket, bytes, (int)SDL_min(length, (usz)SDL_MAX_SINT32));
    return count < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : count == 0 ? MBEDTLS_ERR_SSL_WANT_READ : count;
}

/// @brief Moves a TLS connection's handshake on, once its socket is connected.
static void NetTLSHandshake(NetConnection *connection)
{
    if (connection->tls == nullptr || connection->secured || connection->failed || connection->socket == nullptr || NET_GetConnectionStatus(connection->socket) != NET_SUCCESS)
    {
        return;
    }

    int result = mbedtls_ssl_handshake(connection->tls);

    if (result == 0)
    {
        connection->secured = true;
    }
    else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE)
    {
        char message[128];
        mbedtls_strerror(result, message, sizeof(message));
        ECS_LogWarn(NET.plugin, "The TLS handshake failed: %s", message);
        connection->failed = true;
    }
}

/// @brief Moves a connection on: makes its socket once its name resolves, then, for TLS, moves its handshake on.
static void NetAdvance(NetConnection *connection)
{
    NetTLSHandshake(connection);

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

// a TLS connection checks the server's certificate against the name it was asked for
static NetConnection *NetConnectTLS(const char *host, i32 port)
{
    NetConnection *connection = NetTLSStart() ? NetConnect(host, port) : nullptr;
    mbedtls_ssl_context *tls = connection == nullptr ? nullptr : SDL_malloc(sizeof(mbedtls_ssl_context));

    if (tls == nullptr)
    {
        if (connection != nullptr)
        {
            NET_UnrefAddress(connection->address);
            SDL_free(connection);
        }

        return nullptr;
    }

    mbedtls_ssl_init(tls);
    connection->tls = tls;

    if (mbedtls_ssl_setup(tls, &NET.tlsConfig) != 0 || mbedtls_ssl_set_hostname(tls, host) != 0)
    {
        connection->failed = true;
    }

    mbedtls_ssl_set_bio(tls, connection, NetTLSSend, NetTLSReceive, nullptr);
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
        // a TLS connection waits for its handshake
        return connection->tls == nullptr || connection->secured ? NetState_Connected : NetState_Waiting;
    case NET_WAITING:
        return NetState_Waiting;
    default:
        return NetState_Failed;
    }
}

static bool NetSend(NetConnection *connection, SHUSlice data)
{
    NetAdvance(connection);

    if (connection->tls == nullptr)
    {
        return connection->socket != nullptr && data.size <= (usz)SDL_MAX_SINT32 && NET_WriteToStreamSocket(connection->socket, data.data, (int)data.size);
    }

    // TLS encrypts the data into records, which the socket queues; nothing goes before the handshake is done
    for (usz sent = 0; connection->secured && !connection->failed && sent < data.size;)
    {
        int result = mbedtls_ssl_write(connection->tls, (const u8 *)data.data + sent, data.size - sent);
        connection->failed = result < 0;
        sent += result > 0 ? (usz)result : 0;
    }

    return connection->secured && !connection->failed;
}

static SHUSlice NetReceive(NetConnection *connection, i32 most)
{
    NetAdvance(connection);
    SDL_free(connection->received);
    connection->received = nullptr;

    bool ready = connection->socket != nullptr && (connection->tls == nullptr || (connection->secured && !connection->failed));
    u8 *buffer = !ready || most <= 0 ? nullptr : SDL_malloc((usz)most);
    int count = 0;

    if (buffer != nullptr && connection->tls == nullptr)
    {
        count = NET_ReadFromStreamSocket(connection->socket, buffer, most);
    }
    else if (buffer != nullptr)
    {
        // TLS gives what it decrypted; nothing yet is not an error, and the other side's close is
        count = mbedtls_ssl_read(connection->tls, buffer, (usz)most);
        count = count == MBEDTLS_ERR_SSL_WANT_READ || count == MBEDTLS_ERR_SSL_WANT_WRITE ? 0 : count == 0 ? -1 : count;
    }

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

    if (connection->tls != nullptr)
    {
        mbedtls_ssl_free(connection->tls);
        SDL_free(connection->tls);
    }

    NET_DestroyStreamSocket(connection->socket);
    NET_UnrefAddress(connection->address);
    SDL_free(connection->received);

    if (connection->from != nullptr)
    {
        NetReleaseServer(connection->from);
    }

    SDL_free(connection);
}

/// @brief Waits for TCP connections on a port of every address, with SDL_net.
static NET_Server *NetCreateServer(i32 port)
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

static NetServer *NetListen(i32 port)
{
    NetServer *server = SDL_calloc(1, sizeof(NetServer));

    if (server == nullptr)
    {
        return nullptr;
    }

    mbedtls_x509_crt_init(&server->certificate);
    mbedtls_pk_init(&server->key);
    server->users = 1;
    server->server = NetCreateServer(port);

    if (server->server == nullptr)
    {
        NetReleaseServer(server);
        return nullptr;
    }

    return server;
}

// a TLS server shows its certificate to the clients, and proves it with the certificate's key
static NetServer *NetListenTLS(i32 port, const char *certificate, const char *key)
{
    char *certificatePath = NetFull(certificate);
    char *keyPath = NetFull(key);
    NetServer *server = certificatePath != nullptr && keyPath != nullptr && NetTLSStart() ? NetListen(port) : nullptr;
    mbedtls_ssl_config *tls = server == nullptr ? nullptr : SDL_malloc(sizeof(mbedtls_ssl_config));

    if (tls != nullptr)
    {
        mbedtls_ssl_config_init(tls);
        server->tls = tls;
    }

    if (server != nullptr &&
        (tls == nullptr || mbedtls_x509_crt_parse_file(&server->certificate, certificatePath) != 0 ||
         mbedtls_pk_parse_keyfile(&server->key, keyPath, nullptr, mbedtls_ctr_drbg_random, &NET.random) != 0 ||
         mbedtls_ssl_config_defaults(tls, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
         mbedtls_ssl_conf_own_cert(tls, &server->certificate, &server->key) != 0))
    {
        ECS_LogWarn(NET.plugin, "Cannot serve TLS with the certificate '%s' and the key '%s'", certificatePath, keyPath);
        NetReleaseServer(server);
        server = nullptr;
    }
    else if (server != nullptr)
    {
        mbedtls_ssl_conf_rng(tls, mbedtls_ctr_drbg_random, &NET.random);
    }

    SDL_free(certificatePath);
    SDL_free(keyPath);
    return server;
}

// a TLS server's connection does its handshake as the client's does, and keeps the server's configuration while it lives
static NetConnection *NetAccept(NetServer *server)
{
    NET_StreamSocket *socket = nullptr;

    if (server->server == nullptr || !NET_AcceptClient(server->server, &socket) || socket == nullptr)
    {
        return nullptr;
    }

    NetConnection *connection = SDL_calloc(1, sizeof(NetConnection));
    mbedtls_ssl_context *tls = connection == nullptr || server->tls == nullptr ? nullptr : SDL_malloc(sizeof(mbedtls_ssl_context));

    if (connection == nullptr || (server->tls != nullptr && tls == nullptr))
    {
        NET_DestroyStreamSocket(socket);
        SDL_free(connection);
        return nullptr;
    }

    connection->socket = socket;

    if (tls != nullptr)
    {
        mbedtls_ssl_init(tls);
        connection->tls = tls;
        connection->from = server;
        server->users++;
        connection->failed = mbedtls_ssl_setup(tls, server->tls) != 0;
        mbedtls_ssl_set_bio(tls, connection, NetTLSSend, NetTLSReceive, nullptr);
    }

    return connection;
}

/// @brief The Destroy function of the handle type net.server: it stops listening, and the server is freed once its TLS connections are.
static void NetDestroyServer(void *object)
{
    NetServer *server = object;
    NET_DestroyServer(server->server);
    server->server = nullptr;
    NetReleaseServer(server);
}

static NetUDP *NetUDPOpen(i32 port)
{
    NetUDP *udp = SDL_calloc(1, sizeof(NetUDP));

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
static void NetUDPFlush(NetUDP *udp)
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
static NET_Address *NetUDPAddress(NetUDP *udp, const char *host)
{
    for (usz i = 0; i < NET_UDP_HOSTS; i++)
    {
        if (udp->hosts[i].host == nullptr || SDL_strcmp(udp->hosts[i].host, host) != 0)
        {
            continue;
        }

        // a host whose name failed to resolve is looked up again, as it may resolve now
        if (NET_GetAddressStatus(udp->hosts[i].address) != NET_FAILURE)
        {
            return NET_RefAddress(udp->hosts[i].address);
        }

        SDL_free(udp->hosts[i].host);
        NET_UnrefAddress(udp->hosts[i].address);
        udp->hosts[i].host = nullptr;
        udp->hosts[i].address = nullptr;
        break;
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

static bool NetUDPSendTo(NetUDP *udp, const char *host, i32 port, SHUSlice data)
{
    NetPendingDatagram *datagram = data.size <= (usz)SDL_MAX_SINT32 ? SDL_calloc(1, sizeof(NetPendingDatagram)) : nullptr;
    datagram = datagram == nullptr ? nullptr : (datagram->data = SDL_malloc(SDL_max(data.size, 1)), datagram);

    if (datagram == nullptr || datagram->data == nullptr || (datagram->address = NetUDPAddress(udp, host)) == nullptr)
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
    NetUDPFlush(udp);
    return true;
}

static SHUSlice NetUDPReceiveFrom(NetUDP *udp, const char **retHost, i32 *retPort)
{
    NetUDPFlush(udp);
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

static void NetDestroyUDP(void *object)
{
    NetUDP *udp = object;

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

    NetTLSStop();

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

    // mbedTLS takes memory from SDL's allocator, as the core does
    mbedtls_platform_set_calloc_free(SDL_calloc, SDL_free);
    NET.started = NET_Init();

    if (!NET.started)
    {
        ECS_LogError(plugin, "Cannot start SDL_net: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "connection", NetDestroyConnection), NetFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "server", NetDestroyServer), NetFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "udp", NetDestroyUDP), NetFree(nullptr););

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"connect", (ECSFunction)NetConnect, "handle<net.connection>(string host, int port)", "Start a TCP connection to a host and port"},
        {"connectTLS", (ECSFunction)NetConnectTLS, "handle<net.connection>(string host, int port)", "Start a TLS connection to a host and port, which checks the host's certificate against the system's trusted ones"},
        {"status", (ECSFunction)NetStatus, "int(handle<net.connection> connection)", "Give a connection's state: 1 connected, and for TLS secured, 0 waiting, -1 failed or closed"},
        {"send", (ECSFunction)NetSend, "bool(handle<net.connection> connection, buffer data)", "Send data; it goes out in the background"},
        {"receive", (ECSFunction)NetReceive, "buffer(handle<net.connection> connection, int most)", "Take what has arrived, at most a number of bytes; nothing if nothing has"},
        {"close", (ECSFunction)NetClose, "void(handle<net.connection> connection)", "Close a connection"},
        {"listen", (ECSFunction)NetListen, "handle<net.server>(int port)", "Wait for TCP connections on a port of every address of this computer"},
        {"listenTLS", (ECSFunction)NetListenTLS, "handle<net.server>(int port, string certificate, string key)", "Wait for TLS connections on a port, with a certificate and its key from PEM files"},
        {"trust", (ECSFunction)NetTrust, "bool(string path)", "Trust the certificates of a PEM file too, beside the system's, for later TLS connections"},
        {"accept", (ECSFunction)NetAccept, "handle<net.connection>(handle<net.server> server)", "Take a connection that a server has, if any"},
        {"udp", (ECSFunction)NetUDPOpen, "handle<net.udp>(int port)", "Open a UDP socket on a port of every address of this computer; port 0 picks a free one"},
        {"sendTo", (ECSFunction)NetUDPSendTo, "bool(handle<net.udp> socket, string host, int port, buffer data)", "Send a datagram to a host and port; it leaves once the host's name resolves, and may be lost on the way"},
        {"receiveFrom", (ECSFunction)NetUDPReceiveFrom, "buffer(handle<net.udp> socket, out string host, out int port)", "Take a datagram that has arrived, with the address and port it came from; nothing if none has"},
    };

    // a plugin whose Init fails is not destroyed, so it cleans up here
    const ECSPluginDesc desc = {.Destroy = NetFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc), NetFree(nullptr););

    for (usz i = 0; i < SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), NetFree(nullptr););
    }

    // Lua calls these on a handle, such as handle:status(...); C calls the functions themselves (OpenECS DESIGN 10.6)
    const struct
    {
        const char *type;
        const char *method;
        const char *function;
    } methods[] = {
        {"connection", "status", "status"},
        {"connection", "send", "send"},
        {"connection", "receive", "receive"},
        {"connection", "close", "close"},
        {"server", "accept", "accept"},
        {"udp", "sendTo", "sendTo"},
        {"udp", "receiveFrom", "receiveFrom"},
    };

    for (usz i = 0; i < SDL_arraysize(methods); i++)
    {
        SHU_ReturnResult(ECSHandle_AddMethod(plugin, methods[i].type, methods[i].method, methods[i].function), NetFree(nullptr););
    }

    return SHUResult_Ok;
}
