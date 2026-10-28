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

#define STB_DS_IMPLEMENTATION
#include "stbSDL3.h"

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
    u8 *received;                 // what receive gave last, kept until the next receive
    mbedtls_ssl_context *tls;     // a TLS connection's state, or nullptr for plain TCP
    bool secured;                 // the TLS handshake is done
    struct NetServer *acceptedBy; // the TLS server that accepted it, whose configuration it uses, or nullptr
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
} NetPendingDatagram;

/// @brief A UDP socket: datagrams waiting to be sent, the hosts it resolved, and what it received last.
typedef struct NetUdp
{
    NET_DatagramSocket *socket;
    NetPendingDatagram *pending; // stb_ds array, in the order they were sent
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
    bool netStarted;

    // what every TLS connection shares, made at the first one
    bool tlsStarted;
    mbedtls_ssl_config tlsConfig;
    mbedtls_x509_crt certificates; // the system's trusted certificates
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context random;
} NET = {0};

#pragma region TLS

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

/// @brief Frees what TLS connections share.
static void NetTlsStop(void)
{
    if (NET.tlsStarted)
    {
        mbedtls_ssl_config_free(&NET.tlsConfig);
        mbedtls_x509_crt_free(&NET.certificates);
        mbedtls_ctr_drbg_free(&NET.random);
        mbedtls_entropy_free(&NET.entropy);
        mbedtls_psa_crypto_free();
        NET.tlsStarted = false;
    }
}

/// @brief Makes what TLS connections share, once: the random generator, the trusted certificates and the configuration of a client.
static bool NetTlsStart(void)
{
    if (NET.tlsStarted)
    {
        return true;
    }

    mbedtls_ssl_config_init(&NET.tlsConfig);
    mbedtls_x509_crt_init(&NET.certificates);
    mbedtls_entropy_init(&NET.entropy);
    mbedtls_ctr_drbg_init(&NET.random);
    NET.tlsStarted = true;

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
        NetTlsStop();
        return false;
    }

    mbedtls_ssl_conf_authmode(&NET.tlsConfig, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&NET.tlsConfig, &NET.certificates, nullptr);
    mbedtls_ssl_conf_rng(&NET.tlsConfig, mbedtls_ctr_drbg_random, &NET.random);
    return true;
}

/// @brief Adds the certificates of a PEM file to the trusted ones, which later TLS connections check servers with.
static bool NetTrust(const char *path)
{
    char *fullPath = nullptr;

    // as with the system's file, the certificates that parse are trusted, and those that do not are reported
    int failed = ECSPath_GetFull(&fullPath, path) == SHUResult_Ok && NetTlsStart() ? mbedtls_x509_crt_parse_file(&NET.certificates, fullPath) : -1;
    bool trusted = failed >= 0;

    if (!trusted)
    {
        ECS_LogWarn(NET.plugin, "Cannot read the certificates of '%s'", fullPath != nullptr ? fullPath : path);
    }
    else if (failed > 0)
    {
        ECS_LogWarn(NET.plugin, "%d certificates of '%s' cannot be read; the others are trusted", failed, fullPath);
    }

    ECSPath_Free(fullPath);
    return trusted;
}

/// @brief Gives TLS's bytes to the socket; SDL_net queues them all.
static int NetTlsSend(void *data, const unsigned char *bytes, size_t length)
{
    NetConnection *connection = data;
    return length <= (usz)SDL_MAX_SINT32 && NET_WriteToStreamSocket(connection->socket, bytes, (int)length) ? (int)length : MBEDTLS_ERR_NET_SEND_FAILED;
}

/// @brief Takes the bytes that have arrived for TLS; none yet means TLS waits.
static int NetTlsReceive(void *data, unsigned char *bytes, size_t length)
{
    NetConnection *connection = data;
    int count = NET_ReadFromStreamSocket(connection->socket, bytes, (int)SDL_min(length, (usz)SDL_MAX_SINT32));
    return count < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : count == 0 ? MBEDTLS_ERR_SSL_WANT_READ
                                                                : count;
}

/// @brief Gives a connection the state of a TLS connection with a configuration: a client's, or the server's that accepted it.
/// @return false if out of memory.
static bool NetAddTls(NetConnection *connection, const mbedtls_ssl_config *config)
{
    connection->tls = SDL_malloc(sizeof(mbedtls_ssl_context));

    if (connection->tls == nullptr)
    {
        return false;
    }

    mbedtls_ssl_init(connection->tls);
    connection->failed = connection->failed || mbedtls_ssl_setup(connection->tls, config) != 0;
    mbedtls_ssl_set_bio(connection->tls, connection, NetTlsSend, NetTlsReceive, nullptr);
    return true;
}

/// @brief Moves a TLS connection's handshake on, once its socket is connected.
static void NetTlsHandshake(NetConnection *connection)
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

#pragma endregion TLS

#pragma region Connections

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

/// @brief Moves a connection on: makes its socket once its name resolves, then, for TLS, moves its handshake on.
static void NetAdvance(NetConnection *connection)
{
    NetTlsHandshake(connection);

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

static void *NetConnect(const char *host, i32 port)
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

static void NetConnectionDestroy(void *object)
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

    if (connection->acceptedBy != nullptr)
    {
        NetReleaseServer(connection->acceptedBy);
    }

    SDL_free(connection);
}

// a TLS connection checks the server's certificate against the name it was asked for
static void *NetConnectTls(const char *host, i32 port)
{
    NetConnection *connection = NetTlsStart() ? NetConnect(host, port) : nullptr;

    if (connection == nullptr)
    {
        return nullptr;
    }

    if (!NetAddTls(connection, &NET.tlsConfig))
    {
        NetConnectionDestroy(connection);
        return nullptr;
    }

    connection->failed = connection->failed || mbedtls_ssl_set_hostname(connection->tls, host) != 0;
    return connection;
}

static i32 NetStatus(void *connectionHandle)
{
    NetConnection *connection = connectionHandle;

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

static bool NetSend(void *connectionHandle, SHUSlice data)
{
    NetConnection *connection = connectionHandle;

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

/// @brief Reads what arrived on a connection into a buffer.
/// @return How many bytes it read, 0 when none arrived, or -1 once the other side closed.
static int NetRead(NetConnection *connection, u8 *buffer, i32 maxBytes)
{
    if (connection->tls == nullptr)
    {
        return NET_ReadFromStreamSocket(connection->socket, buffer, maxBytes);
    }

    // TLS gives what it decrypted; nothing yet is not an error, and the other side's close is
    int count = mbedtls_ssl_read(connection->tls, buffer, (usz)maxBytes);
    return count == MBEDTLS_ERR_SSL_WANT_READ || count == MBEDTLS_ERR_SSL_WANT_WRITE ? 0 : count == 0 ? -1
                                                                                                      : count;
}

static SHUSlice NetReceive(void *connectionHandle, i32 maxBytes)
{
    NetConnection *connection = connectionHandle;

    NetAdvance(connection);
    SDL_free(connection->received);
    connection->received = nullptr;

    bool ready = connection->socket != nullptr && (connection->tls == nullptr || (connection->secured && !connection->failed));
    u8 *buffer = !ready || maxBytes <= 0 ? nullptr : SDL_malloc((usz)maxBytes);

    if (buffer == nullptr)
    {
        return cs(nullptr, 0);
    }

    int count = NetRead(connection, buffer, maxBytes);

    // a connection that the other side closed is failed from then on
    if (count < 0)
    {
        connection->failed = true;
        count = 0;
    }

    connection->received = buffer;
    return cs(buffer, (usz)count);
}

static void NetClose(void *connectionHandle)
{
    NetConnection *connection = connectionHandle;

    NET_DestroyStreamSocket(connection->socket);
    connection->socket = nullptr;
    connection->failed = true;
}

#pragma endregion Connections

#pragma region Servers

/// @brief Resolves the IPv4 address that stands for every address of this computer, 0.0.0.0, for SDL_net, which listens on every family of
/// addresses at once and fails if one is missing, such as IPv6.
/// @return The address, to let go of with NET_UnrefAddress, or nullptr if it does not resolve.
static NET_Address *NetResolveAnyIpv4(void)
{
    NET_Address *any = NET_ResolveHostname("0.0.0.0");

    if (any != nullptr && NET_WaitUntilResolved(any, 1000) != NET_SUCCESS)
    {
        NET_UnrefAddress(any);
        return nullptr;
    }

    return any;
}

/// @brief Waits for TCP connections on a port of every address, with SDL_net; IPv4 alone when that fails.
static NET_Server *NetCreateServer(i32 port)
{
    u16 number = (u16)SDL_clamp(port, 0, 65535);
    NET_Server *server = NET_CreateServer(nullptr, number, 0);
    NET_Address *any = server == nullptr ? NetResolveAnyIpv4() : nullptr;

    if (any != nullptr)
    {
        server = NET_CreateServer(any, number, 0);
        NET_UnrefAddress(any);
    }

    if (server == nullptr)
    {
        ECS_LogWarn(NET.plugin, "Cannot listen on port %d: %s", port, SDL_GetError());
    }

    return server;
}

static void *NetListen(i32 port)
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

/// @brief Gives a server the configuration of TLS, with a certificate and its key from PEM files, whose relative paths start at the executable's folder.
/// @return false if the files cannot be read.
static bool NetAddServerTls(NetServer *server, const char *certificatePath, const char *keyPath)
{
    server->tls = SDL_malloc(sizeof(mbedtls_ssl_config));

    if (server->tls == nullptr)
    {
        return false;
    }

    mbedtls_ssl_config_init(server->tls);

    if (mbedtls_x509_crt_parse_file(&server->certificate, certificatePath) != 0 || mbedtls_pk_parse_keyfile(&server->key, keyPath, nullptr, mbedtls_ctr_drbg_random, &NET.random) != 0 ||
        mbedtls_ssl_config_defaults(server->tls, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0 ||
        mbedtls_ssl_conf_own_cert(server->tls, &server->certificate, &server->key) != 0)
    {
        return false;
    }

    mbedtls_ssl_conf_rng(server->tls, mbedtls_ctr_drbg_random, &NET.random);
    return true;
}

// a TLS server shows its certificate to the clients, and proves it with the certificate's key
static void *NetListenTls(i32 port, const char *certificate, const char *key)
{
    char *certificatePath = nullptr;
    char *keyPath = nullptr;
    bool found = ECSPath_GetFull(&certificatePath, certificate) == SHUResult_Ok && ECSPath_GetFull(&keyPath, key) == SHUResult_Ok;
    NetServer *server = found && NetTlsStart() ? NetListen(port) : nullptr;

    if (server != nullptr && !NetAddServerTls(server, certificatePath, keyPath))
    {
        ECS_LogWarn(NET.plugin, "Cannot serve TLS with the certificate '%s' and the key '%s'", certificatePath, keyPath);
        NetReleaseServer(server);
        server = nullptr;
    }

    ECSPath_Free(certificatePath);
    ECSPath_Free(keyPath);
    return server;
}

// a TLS server's connection does its handshake as the client's does, and keeps the server's configuration while it lives
static void *NetAccept(void *serverHandle)
{
    NetServer *server = serverHandle;

    NET_StreamSocket *socket = nullptr;

    if (server->server == nullptr || !NET_AcceptClient(server->server, &socket) || socket == nullptr)
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

    if (server->tls == nullptr)
    {
        return connection;
    }

    if (!NetAddTls(connection, server->tls))
    {
        NetConnectionDestroy(connection);
        return nullptr;
    }

    connection->acceptedBy = server;
    server->users++;
    return connection;
}

/// @brief The Destroy function of the handle type net.server: it stops listening, and the server is freed once its TLS connections are.
static void NetServerDestroy(void *object)
{
    NetServer *server = object;
    NET_DestroyServer(server->server);
    server->server = nullptr;
    NetReleaseServer(server);
}

#pragma endregion Servers

#pragma region UDP

static void *NetOpenUdp(i32 port)
{
    NetUdp *udp = SDL_calloc(1, sizeof(NetUdp));

    if (udp == nullptr)
    {
        return nullptr;
    }

    // as for a server, IPv4 alone is tried when a family of addresses is missing
    u16 number = (u16)SDL_clamp(port, 0, 65535);
    udp->socket = NET_CreateDatagramSocket(nullptr, number, 0);
    NET_Address *any = udp->socket == nullptr ? NetResolveAnyIpv4() : nullptr;

    if (any != nullptr)
    {
        udp->socket = NET_CreateDatagramSocket(any, number, 0);
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

static void NetFreeDatagram(NetPendingDatagram *datagram)
{
    NET_UnrefAddress(datagram->address);
    SDL_free(datagram->data);
}

/// @brief Sends the waiting datagrams whose hosts resolved, in order; one whose host cannot be resolved is dropped.
static void NetUdpFlush(NetUdp *udp)
{
    usz done = 0;

    while (done < arrlenu(udp->pending))
    {
        NetPendingDatagram *datagram = &udp->pending[done];
        NET_Status status = NET_GetAddressStatus(datagram->address);

        if (status == NET_WAITING)
        {
            break;
        }

        if (status == NET_SUCCESS)
        {
            NET_SendDatagram(udp->socket, datagram->address, datagram->port, datagram->data, (int)datagram->size);
        }

        NetFreeDatagram(datagram);
        done++;
    }

    if (done > 0)
    {
        arrdeln(udp->pending, 0, done);
    }
}

/// @brief Gives a host's address, resolving it once per socket.
/// @return A reference the caller owns, or nullptr if the name cannot be resolved.
static NET_Address *NetUdpAddress(NetUdp *udp, const char *host)
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

static bool NetUdpSendTo(void *udpHandle, const char *host, i32 port, SHUSlice data)
{
    NetUdp *udp = udpHandle;

    if (data.size > (usz)SDL_MAX_SINT32)
    {
        return false;
    }

    u8 *copy = SDL_malloc(SDL_max(data.size, 1));
    NET_Address *address = copy == nullptr ? nullptr : NetUdpAddress(udp, host);

    if (address == nullptr)
    {
        SDL_free(copy);
        return false;
    }

    // it waits behind the others, so that datagrams to a host leave in the order they were sent
    SDL_memcpy(copy, data.data, data.size);
    arrput(udp->pending, ((NetPendingDatagram){.address = address, .port = (u16)SDL_clamp(port, 0, 65535), .data = copy, .size = data.size}));
    NetUdpFlush(udp);
    return true;
}

static SHUSlice NetUdpReceiveFrom(void *udpHandle, const char **retHost, i32 *retPort)
{
    NetUdp *udp = udpHandle;

    NetUdpFlush(udp);
    NET_DestroyDatagram(udp->received);
    udp->received = nullptr;
    *retHost = nullptr;
    *retPort = 0;

    if (!NET_ReceiveDatagram(udp->socket, &udp->received) || udp->received == nullptr)
    {
        return cs(nullptr, 0);
    }

    *retHost = NET_GetAddressString(udp->received->addr);
    *retPort = udp->received->port;
    return cs(udp->received->buf, (usz)udp->received->buflen);
}

static void NetUdpDestroy(void *object)
{
    NetUdp *udp = object;

    for (usz i = 0; i < arrlenu(udp->pending); i++)
    {
        NetFreeDatagram(&udp->pending[i]);
    }

    arrfree(udp->pending);

    for (usz i = 0; i < NET_UDP_HOSTS; i++)
    {
        SDL_free(udp->hosts[i].host);
        NET_UnrefAddress(udp->hosts[i].address);
    }

    NET_DestroyDatagram(udp->received);
    NET_DestroyDatagramSocket(udp->socket);
    SDL_free(udp);
}

#pragma endregion UDP

static void NetFree(void *data)
{
    (void)data;

    NetTlsStop();

    if (NET.netStarted)
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
    NET.netStarted = NET_Init();

    if (!NET.netStarted)
    {
        ECS_LogError(plugin, "Cannot start SDL_net: %s", SDL_GetError());
        return SHUResult_ErrInternal;
    }

    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "connection", NetConnectionDestroy), NetFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "server", NetServerDestroy), NetFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "udp", NetUdpDestroy), NetFree(nullptr););

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"connect", (ECSFunction)NetConnect, "handle<net.connection>(string host, int port)",
         "Start a TCP connection to a host and port\n"
         "@param host The host's name or address; it resolves in the background\n"
         "@param port The port, from 0 to 65535\n"
         "@return The connection, whose status tells when it connects or fails"},
        {"connectTLS", (ECSFunction)NetConnectTls, "handle<net.connection>(string host, int port)",
         "Start a TLS connection to a host and port, which checks the host's certificate against the system's trusted ones\n"
         "@param host The host's name or address; it resolves in the background, and the certificate must be for it\n"
         "@param port The port, from 0 to 65535\n"
         "@return The connection, whose status is 1 once the handshake is done; nothing if TLS cannot start"},
        {"status", (ECSFunction)NetStatus, "int(handle<net.connection> connection)",
         "Give a connection's state: 1 connected, and for TLS secured, 0 waiting, -1 failed or closed\n"
         "@param connection The connection\n"
         "@return 1 once it is connected, and for TLS its handshake done; 0 while it waits; -1 if it failed or closed"},
        {"send", (ECSFunction)NetSend, "bool(handle<net.connection> connection, buffer data)",
         "Send data; it goes out in the background\n"
         "@param connection The connection\n"
         "@param data The bytes to send; they are copied\n"
         "@return False if it has no socket yet, it is closed, or for TLS its handshake is not done"},
        {"receive", (ECSFunction)NetReceive, "buffer(handle<net.connection> connection, int maxBytes)",
         "Take what has arrived, at most a number of bytes; nothing if nothing has\n"
         "@param connection The connection\n"
         "@param maxBytes The most bytes to take\n"
         "@return What arrived, kept until the next receive on the connection; empty if nothing has"},
        {"close", (ECSFunction)NetClose, "void(handle<net.connection> connection)",
         "Close a connection\n"
         "@param connection The connection; its status is -1 after"},
        {"listen", (ECSFunction)NetListen, "handle<net.server>(int port)",
         "Wait for TCP connections on a port of every address of this computer\n"
         "@param port The port, from 0 to 65535\n"
         "@return The server, or nothing if it cannot listen"},
        {"listenTLS", (ECSFunction)NetListenTls, "handle<net.server>(int port, string certificate, string key)",
         "Wait for TLS connections on a port, with a certificate and its key from PEM files\n"
         "@param port The port, from 0 to 65535\n"
         "@param certificate The path of the certificate's PEM file; a relative path starts at the executable's folder\n"
         "@param key The path of the private key's PEM file; a relative path starts at the executable's folder\n"
         "@return The server, or nothing if it cannot listen or cannot read the certificate or key"},
        {"trust", (ECSFunction)NetTrust, "bool(string path)",
         "Trust the certificates of a PEM file too, beside the system's, for later TLS connections\n"
         "@param path The path of the PEM file; a relative path starts at the executable's folder\n"
         "@return False if the file cannot be read"},
        {"accept", (ECSFunction)NetAccept, "handle<net.connection>(handle<net.server> server)",
         "Take a connection that a server has, if any\n"
         "@param server The server, as listen or listenTLS gives it\n"
         "@return A connection that a client made, or nothing if none waits"},
        {"udp", (ECSFunction)NetOpenUdp, "handle<net.udp>(int port)",
         "Open a UDP socket on a port of every address of this computer; port 0 picks a free one\n"
         "@param port The port, from 0 to 65535; 0 picks a free one\n"
         "@return The socket, or nothing if it cannot open"},
        {"sendTo", (ECSFunction)NetUdpSendTo, "bool(handle<net.udp> socket, string host, int port, buffer data)",
         "Send a datagram to a host and port; it leaves once the host's name resolves, and may be lost on the way\n"
         "@param socket The UDP socket\n"
         "@param host The host's name or address; the socket keeps the last 8 it resolved\n"
         "@param port The host's port, from 0 to 65535\n"
         "@param data The datagram's bytes; they are copied\n"
         "@return False if the host's name cannot start to resolve"},
        {"receiveFrom", (ECSFunction)NetUdpReceiveFrom, "buffer(handle<net.udp> socket, out string host, out int port)",
         "Take a datagram that has arrived, with the address and port it came from; nothing if none has\n"
         "@param socket The UDP socket\n"
         "@param host The address the datagram came from, kept until the next receiveFrom; nothing if none has arrived\n"
         "@param port The port the datagram came from; 0 if none has arrived\n"
         "@return The datagram, kept until the next receiveFrom on the socket; empty if none has arrived"},
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
