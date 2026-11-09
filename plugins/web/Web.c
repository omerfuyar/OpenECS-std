// The web standard plugin: HTTP and HTTPS requests, and WebSocket connections, that never block the program (DESIGN 11).
// libcurl makes them, with TLS from mbedTLS, HTTP/2 from nghttp2, and compressed answers from zlib and zstd; all are compiled into this plugin (DESIGN 1). One thread of the plugin runs every request and connection.
// A plugin gives a function that gets the request once it is done, or asks for its state and takes its answer when it wants.
// A WebSocket connection is a request that goes on: the plugin asks for its state, and sends and takes messages when it wants.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#include <curl/curl.h>
#include <mbedtls/platform.h>

// Windows keeps its trusted certificates in a store of its own, which crypt32 reads
#if defined(SDL_PLATFORM_WINDOWS)
#include <windows.h>
#include <wincrypt.h>
#endif

#define STB_DS_IMPLEMENTATION
#include "stbSDL3.h"

#pragma region Source Only

/// @brief The most redirects a request follows.
#define WEB_REDIRECTS 10

/// @brief How often, in seconds, the main thread looks for requests whose function is to be called.
#define WEB_DONE_INTERVAL 0.02

struct WebRequest;

/// @brief A WebSocket message.
typedef struct WebMessage
{
    u8 *data; // stb_ds array of its bytes
    bool text;
} WebMessage;

/// @brief The function that gets a request once it is done: void(handle<web.request>). It is a kept callback (OpenECS DESIGN 10.4).
typedef void (*WebDoneFunction)(struct WebRequest *request);

/// @brief A request, from the main thread to the plugin's thread and back. The lock guards done, status and the lists; the rest belongs to one thread at a time:
/// the main thread until the request is queued, the plugin's thread until it is done, and the main thread again then.
typedef struct WebRequest
{
    CURL *easy;
    struct curl_slist *headers;
    char error[CURL_ERROR_SIZE];
    u8 *body;   // stb_ds array of what has arrived
    i32 status; // the HTTP status once done, or -1 if it failed
    bool done;
    bool forgotten;       // freed by its owner while it runs; the plugin's thread frees it
    bool running;         // given to the multi handle, on the plugin's thread
    WebDoneFunction Done; // main thread: the function to call once it is done, or nullptr once called

    // a WebSocket connection's; the lock guards open and the lists, and the rest belongs to the thread that the note names
    bool socket;
    bool open;           // the server took the connection
    WebMessage **inbox;  // stb_ds array of the messages that arrived, oldest first
    WebMessage **outbox; // stb_ds array of the messages to send, oldest first
    bool waking;         // in the list of connections with messages to send
    bool paused;         // plugin's thread: nothing was left to send, so curl waits until it is woken
    WebMessage *partial; // plugin's thread: the message that is arriving
    WebMessage *sending; // plugin's thread: the message that is going out
    usz sent;            // plugin's thread: how much of it went
    WebMessage *taken;   // main thread: the message that the last receive gave, kept until the next one
} WebRequest;

static struct
{
    ECSPlugin plugin;
    bool curlStarted;   // curl_global_init worked
    CURLM *multi;       // used only on the plugin's thread, except to wake it
    SDL_Thread *thread; // started by the first request
    SDL_Mutex *lock;
    WebRequest **added;     // stb_ds array of the requests waiting for the plugin's thread
    WebRequest **forgotten; // stb_ds array of the running requests that their owner freed
    WebRequest **waking;    // stb_ds array of the connections with messages to send
    bool quit;
    const char *certificates; // the system's file of trusted certificates, or nullptr
    char *storeCertificates;  // on Windows without such a file, stb_ds array of the certificates of its store as PEM text that ends with a zero, or nullptr
    WebRequest **all;         // main thread: stb_ds array of every request that is not freed, oldest first
    ECSTimer timer;           // main thread: calls the functions of the requests that are done, while some wait
} WEB = {0};

/// @brief Removes a request from a list, an stb_ds array, if it is there.
static void WebRemove(WebRequest ***list, WebRequest *request)
{
    for (usz i = 0; i < arrlenu(*list); i++)
    {
        if ((*list)[i] == request)
        {
            arrdel(*list, i);
            return;
        }
    }
}

/// @brief Adds bytes at the end of an stb_ds array.
static void WebAppendBytes(u8 **bytes, const void *data, usz size)
{
    if (size > 0)
    {
        SDL_memcpy(arraddnptr(*bytes, size), data, size);
    }
}

static void WebFreeMessage(WebMessage *message)
{
    if (message != nullptr)
    {
        arrfree(message->data);
        SDL_free(message);
    }
}

static void WebFreeMessages(WebMessage **messages)
{
    for (usz i = 0; i < arrlenu(messages); i++)
    {
        WebFreeMessage(messages[i]);
    }

    arrfree(messages);
}

/// @brief Keeps what arrives, on the plugin's thread.
static size_t WebWrite(char *data, size_t size, size_t count, void *user)
{
    WebRequest *request = user;
    WebAppendBytes(&request->body, data, size * count);
    return size * count;
}

/// @brief Keeps a piece of a WebSocket message that arrives, on the plugin's thread; a whole message goes to the inbox.
static size_t WebSocketWrite(char *data, size_t size, size_t count, void *user)
{
    WebRequest *request = user;
    usz length = size * count;
    const struct curl_ws_frame *frame = curl_ws_meta(request->easy);

    // curl answers pings, and ends the connection when it closes; only text and binary frames are messages
    if (frame == nullptr || (frame->flags & (CURLWS_TEXT | CURLWS_BINARY)) == 0)
    {
        return length;
    }

    if (request->partial == nullptr)
    {
        request->partial = SDL_calloc(1, sizeof(WebMessage));

        if (request->partial == nullptr)
        {
            return CURL_WRITEFUNC_ERROR;
        }

        request->partial->text = (frame->flags & CURLWS_TEXT) != 0;
    }

    WebAppendBytes(&request->partial->data, data, length);

    // the last piece of the last frame of a message ends it; CURLWS_CONT marks every frame but the last
    if (frame->bytesleft == 0 && (frame->flags & CURLWS_CONT) == 0)
    {
        SDL_LockMutex(WEB.lock);
        arrput(request->inbox, request->partial);
        SDL_UnlockMutex(WEB.lock);
        request->partial = nullptr;
    }

    return length;
}

/// @brief Gives curl the next piece of the messages to send, on the plugin's thread. With none left, it pauses, until a send wakes the connection.
static size_t WebSocketRead(char *buffer, size_t size, size_t count, void *user)
{
    WebRequest *request = user;
    usz room = size * count;

    // curl asks for data once the server took the connection
    SDL_LockMutex(WEB.lock);
    request->open = true;

    if (request->sending == nullptr && arrlenu(request->outbox) > 0)
    {
        request->sending = request->outbox[0];
        arrdel(request->outbox, 0);
        request->sent = 0;
    }

    SDL_UnlockMutex(WEB.lock);
    WebMessage *message = request->sending;

    if (message == nullptr)
    {
        request->paused = true;
        return CURL_READFUNC_PAUSE;
    }

    // a message is one frame, which starts with its first piece; an empty one is the frame's head alone
    usz messageSize = arrlenu(message->data);

    if (request->sent == 0 && curl_ws_start_frame(request->easy, message->text ? CURLWS_TEXT : CURLWS_BINARY, (curl_off_t)messageSize) != CURLE_OK)
    {
        return CURL_READFUNC_ABORT;
    }

    usz length = SDL_min(room, messageSize - request->sent);
    SDL_memcpy(buffer, message->data + request->sent, length);
    request->sent += length;

    if (request->sent == messageSize)
    {
        WebFreeMessage(message);
        request->sending = nullptr;
    }

    return length;
}

/// @brief Frees a request's memory, which no thread uses any longer.
static void WebFreeMemory(WebRequest *request)
{
    WebFreeMessages(request->inbox);
    WebFreeMessages(request->outbox);
    WebFreeMessage(request->partial);
    WebFreeMessage(request->sending);
    WebFreeMessage(request->taken);
    curl_easy_cleanup(request->easy);
    curl_slist_free_all(request->headers);
    arrfree(request->body);
    SDL_free(request);
}

/// @brief Frees a request on the main thread: it leaves the lists, its function is released without a call, and one that runs stops.
static void WebDelete(WebRequest *request)
{
    WebRemove(&WEB.all, request);
    ECSService_ReleaseCallback((ECSFunction)request->Done);
    request->Done = nullptr;

    // a request that runs is freed by the plugin's thread, which is the one that may touch it now; one that waits in the queue leaves it,
    // and so does a connection that waits to be woken
    SDL_LockMutex(WEB.lock);
    bool running = request->running && !request->done;

    if (running)
    {
        request->forgotten = true;
        arrput(WEB.forgotten, request);
    }

    WebRemove(&WEB.added, request);
    WebRemove(&WEB.waking, request);
    SDL_UnlockMutex(WEB.lock);

    if (running)
    {
        curl_multi_wakeup(WEB.multi);
        return;
    }

    WebFreeMemory(request);
}

/// @brief The Destroy function of the handle types web.request and web.socket. A request whose function waits goes on, and its function gets it in a new handle.
static void WebRequestDestroy(void *object)
{
    WebRequest *request = object;

    if (request->Done == nullptr)
    {
        WebDelete(request);
    }
}

/// @brief Takes the requests that are queued and forgotten, and wakes the connections with messages to send, on the plugin's thread.
/// @return false once the plugin quits.
static bool WebTakeQueues(void)
{
    SDL_LockMutex(WEB.lock);
    WebRequest **added = WEB.added;
    WebRequest **forgotten = WEB.forgotten;
    bool quit = WEB.quit;
    WEB.added = nullptr;
    WEB.forgotten = nullptr;

    for (usz i = 0; i < arrlenu(added); i++)
    {
        added[i]->running = true;
    }

    // a connection is woken only by the thread that runs it; one that is not paused sends its messages anyway, and one that ended has nothing to wake
    // the lock stays held, so the main thread cannot free one of them meanwhile; SDL's mutexes are recursive, so curl's callbacks still take it
    for (usz i = 0; i < arrlenu(WEB.waking); i++)
    {
        WebRequest *request = WEB.waking[i];
        request->waking = false;

        if (request->paused && request->running && !request->done)
        {
            request->paused = false;
            curl_easy_pause(request->easy, CURLPAUSE_CONT);
        }
    }

    arrfree(WEB.waking);
    SDL_UnlockMutex(WEB.lock);

    for (usz i = 0; i < arrlenu(added); i++)
    {
        if (curl_multi_add_handle(WEB.multi, added[i]->easy) != CURLM_OK)
        {
            SDL_LockMutex(WEB.lock);
            SDL_strlcpy(added[i]->error, "The request cannot start", sizeof(added[i]->error));
            added[i]->status = -1;
            added[i]->done = true;
            SDL_UnlockMutex(WEB.lock);
        }
    }

    for (usz i = 0; i < arrlenu(forgotten); i++)
    {
        curl_multi_remove_handle(WEB.multi, forgotten[i]->easy);
        WebFreeMemory(forgotten[i]);
    }

    arrfree(added);
    arrfree(forgotten);
    return !quit;
}

/// @brief The plugin's thread: runs every request, and marks each one done when it ends.
static int WebRun(void *data)
{
    (void)data;

    while (WebTakeQueues())
    {
        int running = 0;
        curl_multi_perform(WEB.multi, &running);

        CURLMsg *message = nullptr;
        int left = 0;

        while ((message = curl_multi_info_read(WEB.multi, &left)) != nullptr)
        {
            WebRequest *request = nullptr;

            if (message->msg != CURLMSG_DONE || curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, (char **)&request) != CURLE_OK || request == nullptr)
            {
                continue;
            }

            long status = 0;
            curl_easy_getinfo(request->easy, CURLINFO_RESPONSE_CODE, &status);
            CURLcode result = message->data.result;
            curl_multi_remove_handle(WEB.multi, request->easy);

            SDL_LockMutex(WEB.lock);

            if (result != CURLE_OK && request->error[0] == '\0')
            {
                SDL_strlcpy(request->error, curl_easy_strerror(result), sizeof(request->error));
            }

            // a connection that ends without an error was closed by the server
            if (request->socket && result == CURLE_OK)
            {
                SDL_strlcpy(request->error, request->open ? "The server closed the connection" : "The server did not take the connection", sizeof(request->error));
            }

            request->status = result == CURLE_OK && !request->socket ? (i32)status : -1;
            request->done = true;
            SDL_UnlockMutex(WEB.lock);
        }

        // waits until a request has news, a new one comes, or a second passes
        curl_multi_poll(WEB.multi, nullptr, 0, 1000, nullptr);
    }

    return 0;
}

/// @brief Starts the plugin's thread, once.
static bool WebStartThread(void)
{
    if (WEB.thread == nullptr)
    {
        WEB.thread = SDL_CreateThread(WebRun, "web", nullptr);

        if (WEB.thread == nullptr)
        {
            ECS_LogError(WEB.plugin, "Cannot start the thread of requests: %s", SDL_GetError());
        }
    }

    return WEB.thread != nullptr;
}

/// @brief Gives the system's file of trusted certificates, which HTTPS checks servers with: SSL_CERT_FILE, or the file of a known distribution.
static const char *WebFindCertificates(void)
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
/// @brief Gives the certificates of Windows' store of trusted root certificates as PEM text that ends with a zero, which libcurl takes as a blob, in
/// an stb_ds array, or nullptr. Free it with arrfree.
static char *WebReadWindowsCertificates(void)
{
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    char *text = nullptr;

    if (store == nullptr)
    {
        return nullptr;
    }

    // each call frees the certificate it is given, and the last one gives nullptr
    for (PCCERT_CONTEXT certificate = CertEnumCertificatesInStore(store, nullptr); certificate != nullptr; certificate = CertEnumCertificatesInStore(store, certificate))
    {
        // the first call gives the size with the zero; the second writes the text, between BEGIN and END CERTIFICATE lines, and gives its size without it
        DWORD size = 0;

        if (CryptBinaryToStringA(certificate->pbCertEncoded, certificate->cbCertEncoded, CRYPT_STRING_BASE64HEADER, nullptr, &size))
        {
            usz length = arrlenu(text);
            char *room = arraddnptr(text, (usz)size);
            DWORD written = size;
            bool converted = CryptBinaryToStringA(certificate->pbCertEncoded, certificate->cbCertEncoded, CRYPT_STRING_BASE64HEADER, room, &written);
            arrsetlen(text, length + (converted ? (usz)written : 0));
        }
    }

    CertCloseStore(store, 0);

    if (arrlenu(text) == 0)
    {
        arrfree(text);
        return nullptr;
    }

    arrput(text, '\0');
    return text;
}
#endif

/// @brief Sets the trusted certificates that a request checks its server with.
static bool WebSetCertificates(CURL *easy)
{
    if (WEB.storeCertificates != nullptr)
    {
        struct curl_blob blob = {.data = WEB.storeCertificates, .len = arrlenu(WEB.storeCertificates), .flags = CURL_BLOB_NOCOPY};
        return curl_easy_setopt(easy, CURLOPT_CAINFO_BLOB, &blob) == CURLE_OK;
    }

    return WEB.certificates == nullptr || curl_easy_setopt(easy, CURLOPT_CAINFO, WEB.certificates) == CURLE_OK;
}

/// @brief Sets a request's method and body: a body makes it a POST, unless the method names another.
static bool WebSetMethod(CURL *easy, const char *method, SHUSlice body)
{
    bool configured = true;

    if (body.size > 0 || SDL_strcasecmp(method, "POST") == 0)
    {
        configured = curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size) == CURLE_OK &&
                     curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, body.size > 0 ? (const char *)body.data : "") == CURLE_OK;
    }

    if (SDL_strcasecmp(method, "HEAD") == 0)
    {
        return configured && curl_easy_setopt(easy, CURLOPT_NOBODY, 1L) == CURLE_OK;
    }

    // curl sends a body with POST, so a GET with a body names its method
    if ((SDL_strcasecmp(method, "GET") == 0 && body.size == 0) || SDL_strcasecmp(method, "POST") == 0)
    {
        return configured;
    }

    return configured && curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method) == CURLE_OK;
}

/// @brief The timeout of the setting web.timeout, in milliseconds.
static long WebTimeout(void)
{
    f64 timeout = ECSValue_GetNumber(ECSSetting_Get("web.timeout"), 30.0);
    return (long)(SDL_max(timeout, 0.0) * 1000.0);
}

/// @brief Makes a request or a WebSocket connection to a URL, with headers, a list of texts, and what every one of them sets: where its answer goes,
/// its error, its user agent and the trusted certificates. The caller sets the rest, and queues it.
/// @return The request, or nullptr if it cannot be made.
static WebRequest *WebNewTransfer(const char *url, const ECSValue *headers, curl_write_callback Write)
{
    WebRequest *request = SDL_calloc(1, sizeof(WebRequest));

    if (request == nullptr)
    {
        return nullptr;
    }

    request->easy = curl_easy_init();

    if (request->easy == nullptr || !WebStartThread())
    {
        WebFreeMemory(request);
        return nullptr;
    }

    for (usz i = 0; i < ECSValue_GetListCount(headers); i++)
    {
        struct curl_slist *list = curl_slist_append(request->headers, ECSValue_GetString(ECSValue_GetListItem(headers, i), ""));

        if (list == nullptr)
        {
            WebFreeMemory(request);
            return nullptr;
        }

        request->headers = list;
    }

    CURL *easy = request->easy;
    bool configured = curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK && curl_easy_setopt(easy, CURLOPT_PRIVATE, request) == CURLE_OK &&
                      curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, request->error) == CURLE_OK && curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, Write) == CURLE_OK &&
                      curl_easy_setopt(easy, CURLOPT_WRITEDATA, request) == CURLE_OK && curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
                      curl_easy_setopt(easy, CURLOPT_USERAGENT, "OpenECS/" OPENECS_VERSION) == CURLE_OK && WebSetCertificates(easy) &&
                      curl_easy_setopt(easy, CURLOPT_HTTPHEADER, request->headers) == CURLE_OK;

    if (!configured)
    {
        WebFreeMemory(request);
        return nullptr;
    }

    return request;
}

/// @brief Gives a request to the plugin's thread, which runs it.
static void WebQueue(WebRequest *request)
{
    arrput(WEB.all, request);
    SDL_LockMutex(WEB.lock);
    arrput(WEB.added, request);
    SDL_UnlockMutex(WEB.lock);
    curl_multi_wakeup(WEB.multi);
}

static bool WebDone(WebRequest *request);

/// @brief Calls the functions of the requests that are done, one at a time, because a function may free requests.
static void WebCallDone(void *data)
{
    (void)data;
    bool waiting = true;

    while (waiting)
    {
        WebRequest *request = nullptr;
        waiting = false;

        for (usz i = 0; request == nullptr && i < arrlenu(WEB.all); i++)
        {
            waiting = waiting || WEB.all[i]->Done != nullptr;
            request = WEB.all[i]->Done != nullptr && WebDone(WEB.all[i]) ? WEB.all[i] : nullptr;
        }

        if (request == nullptr)
        {
            break;
        }

        WebDoneFunction Done = request->Done;
        request->Done = nullptr;
        Done(request);
        ECSService_ReleaseCallback((ECSFunction)Done);
        waiting = true;
    }

    // the timer runs only while a function waits
    if (!waiting && WEB.timer != nullptr)
    {
        ECSTimer_Stop(&WEB.timer);
    }
}

static void *WebNewRequest(const char *method, const char *url, const ECSValue *headers, SHUSlice body, WebDoneFunction Done)
{
    WebRequest *request = WebNewTransfer(url, headers, WebWrite);

    if (request == nullptr)
    {
        ECS_LogWarn(WEB.plugin, "Cannot make a request to '%s'", url);
        ECSService_ReleaseCallback((ECSFunction)Done);
        return nullptr;
    }

    CURL *easy = request->easy;
    bool configured = curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK && curl_easy_setopt(easy, CURLOPT_MAXREDIRS, (long)WEB_REDIRECTS) == CURLE_OK &&
                      curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https") == CURLE_OK && curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https") == CURLE_OK &&
                      curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, WebTimeout()) == CURLE_OK && WebSetMethod(easy, method, body) &&
                      curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "") == CURLE_OK &&
                      (Done == nullptr || WEB.timer != nullptr || ECSTimer_Start(WEB.plugin, &WEB.timer, WEB_DONE_INTERVAL, true, WebCallDone, nullptr) == SHUResult_Ok);

    if (!configured)
    {
        ECS_LogWarn(WEB.plugin, "Cannot make a request to '%s'", url);
        WebFreeMemory(request);
        ECSService_ReleaseCallback((ECSFunction)Done);
        return nullptr;
    }

    request->Done = Done;
    WebQueue(request);
    return request;
}

static void *WebGet(const char *url, WebDoneFunction Done)
{
    return WebNewRequest("GET", url, nullptr, cs(nullptr, 0), Done);
}

static i32 WebStatus(void *requestHandle)
{
    WebRequest *request = requestHandle;

    SDL_LockMutex(WEB.lock);
    i32 status = request->done ? request->status : 0;
    SDL_UnlockMutex(WEB.lock);
    return status;
}

static bool WebDone(WebRequest *request)
{
    return WebStatus(request) != 0;
}

static SHUSlice WebBody(void *requestHandle)
{
    WebRequest *request = requestHandle;

    return WebDone(request) ? cs(request->body, arrlenu(request->body)) : cs(nullptr, 0);
}

static const char *WebHeader(void *requestHandle, const char *name)
{
    WebRequest *request = requestHandle;

    struct curl_header *header = nullptr;

    // -1 is the last response, after the redirects
    if (!WebDone(request) || curl_easy_header(request->easy, name, 0, CURLH_HEADER, -1, &header) != CURLHE_OK)
    {
        return nullptr;
    }

    return header->value;
}

static const char *WebError(void *requestHandle)
{
    WebRequest *request = requestHandle;

    return WebDone(request) ? request->error : "";
}

/// @brief Frees a request now, instead of when no one uses its handle; one that runs stops, and its function is not called.
static void WebFreeRequest(void *request)
{
    ECSHandle_Forget(request);
    WebDelete(request);
}

/// @brief Starts a WebSocket connection to a ws:// or wss:// URL, with headers, a list of texts.
static void *WebOpenSocket(const char *url, const ECSValue *headers)
{
    // the connection goes on until one side closes it, so the timeout limits only how long it takes to connect;
    // curl asks the read function for messages to send, as for an upload
    WebRequest *request = WebNewTransfer(url, headers, WebSocketWrite);

    if (request == nullptr)
    {
        ECS_LogWarn(WEB.plugin, "Cannot connect to '%s'", url);
        return nullptr;
    }

    CURL *easy = request->easy;
    bool configured = curl_easy_setopt(easy, CURLOPT_READFUNCTION, WebSocketRead) == CURLE_OK && curl_easy_setopt(easy, CURLOPT_READDATA, request) == CURLE_OK &&
                      curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L) == CURLE_OK && curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "ws,wss") == CURLE_OK &&
                      curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, WebTimeout()) == CURLE_OK;

    if (!configured)
    {
        ECS_LogWarn(WEB.plugin, "Cannot connect to '%s'", url);
        WebFreeMemory(request);
        return nullptr;
    }

    request->socket = true;
    WebQueue(request);
    return request;
}

static i32 WebSocketStatus(void *socketHandle)
{
    WebRequest *socket = socketHandle;

    SDL_LockMutex(WEB.lock);
    i32 status = socket->done ? -1 : socket->open ? 1
                                                  : 0;
    SDL_UnlockMutex(WEB.lock);
    return status;
}

/// @brief Queues a message to send, as text or as bytes. It goes once the connection is open.
/// @return false if the connection failed or closed, or memory runs out.
static bool WebSocketSend(void *socketHandle, SHUSlice data, bool text)
{
    WebRequest *socket = socketHandle;

    if (WebSocketStatus(socket) < 0)
    {
        return false;
    }

    WebMessage *message = SDL_calloc(1, sizeof(WebMessage));

    if (message == nullptr)
    {
        return false;
    }

    message->text = text;
    WebAppendBytes(&message->data, data.data, data.size);

    // the plugin's thread wakes the connection, since curl wants it woken from the thread that runs it
    SDL_LockMutex(WEB.lock);
    arrput(socket->outbox, message);

    if (!socket->waking)
    {
        socket->waking = true;
        arrput(WEB.waking, socket);
    }

    SDL_UnlockMutex(WEB.lock);
    curl_multi_wakeup(WEB.multi);
    return true;
}

/// @brief Takes the oldest message that arrived, and whether it is "text" or "binary"; nothing if none waits. The message is kept until the next receive.
static SHUSlice WebSocketReceive(void *socketHandle, const char **retType)
{
    WebRequest *socket = socketHandle;

    WebFreeMessage(socket->taken);
    socket->taken = nullptr;
    *retType = nullptr;

    SDL_LockMutex(WEB.lock);

    if (arrlenu(socket->inbox) > 0)
    {
        socket->taken = socket->inbox[0];
        arrdel(socket->inbox, 0);
    }

    SDL_UnlockMutex(WEB.lock);

    if (socket->taken == nullptr)
    {
        return cs(nullptr, 0);
    }

    *retType = socket->taken->text ? "text" : "binary";
    return cs(socket->taken->data, arrlenu(socket->taken->data));
}

static const char *WebSocketError(void *socketHandle)
{
    WebRequest *socket = socketHandle;

    return WebSocketStatus(socket) < 0 ? socket->error : "";
}

/// @brief Stops the plugin's thread and frees the requests left; the Destroy function of the plugin.
static void WebFree(void *data)
{
    (void)data;

    if (WEB.thread != nullptr)
    {
        SDL_LockMutex(WEB.lock);
        WEB.quit = true;
        SDL_UnlockMutex(WEB.lock);
        curl_multi_wakeup(WEB.multi);
        SDL_WaitThread(WEB.thread, nullptr);
    }

    // the handles are destroyed before the plugins, so the requests left are those whose functions wait, and those that C plugins keep
    if (WEB.timer != nullptr)
    {
        ECSTimer_Stop(&WEB.timer);
    }

    for (usz i = 0; i < arrlenu(WEB.all); i++)
    {
        ECSService_ReleaseCallback((ECSFunction)WEB.all[i]->Done);
        curl_multi_remove_handle(WEB.multi, WEB.all[i]->easy);
        WebFreeMemory(WEB.all[i]);
    }

    curl_multi_cleanup(WEB.multi);

    if (WEB.curlStarted)
    {
        curl_global_cleanup();
    }

    SDL_DestroyMutex(WEB.lock);
    arrfree(WEB.all);
    arrfree(WEB.added);
    arrfree(WEB.forgotten);
    arrfree(WEB.waking);
    arrfree(WEB.storeCertificates);
    SDL_zero(WEB);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    WEB.plugin = plugin;

    // curl and mbedTLS take memory from SDL's allocator, as the core does
    mbedtls_platform_set_calloc_free(SDL_calloc, SDL_free);
    WEB.curlStarted = curl_global_init_mem(CURL_GLOBAL_DEFAULT, SDL_malloc, SDL_free, SDL_realloc, SDL_strdup, SDL_calloc) == CURLE_OK;
    WEB.multi = WEB.curlStarted ? curl_multi_init() : nullptr;
    WEB.lock = SDL_CreateMutex();

    if (WEB.multi == nullptr || WEB.lock == nullptr)
    {
        ECS_LogError(plugin, "Cannot start libcurl");
        WebFree(nullptr);
        return SHUResult_ErrInternal;
    }

    // a file of trusted certificates comes first; Windows has none, so its store is read when SSL_CERT_FILE names none
    WEB.certificates = WebFindCertificates();

#if defined(SDL_PLATFORM_WINDOWS)
    WEB.storeCertificates = WEB.certificates == nullptr ? WebReadWindowsCertificates() : nullptr;
#endif

    if (WEB.certificates == nullptr && WEB.storeCertificates == nullptr)
    {
        ECS_LogWarn(plugin, "No trusted certificates are found, so HTTPS requests fail; SSL_CERT_FILE can name a file of them");
    }

    const ECSSettingDesc timeout = {
        .name = "timeout",
        .type = ECSSettingType_Number,
        .description = "Seconds a request may take before it fails; 0 for no limit",
        .defaultNumber = 30.0,
    };

    const struct
    {
        const char *name;
        ECSFunction function;
        const char *signature;
        const char *description;
    } functions[] = {
        {"get", (ECSFunction)WebGet, "handle<web.request>(string url, keep fn<void(handle<web.request> request)> done)",
         "Start a GET request to a URL; done, if given, gets the request once it is done\n"
         "@param url The URL, http:// or https://; one with another scheme fails\n"
         "@param done Called once, on the main thread, when the request is done or failed; its request is the request, in a new handle if the first was dropped; not called if the request is freed first; nothing for no call\n"
         "@return The request, or nothing if it cannot be made"},
        {"request", (ECSFunction)WebNewRequest, "handle<web.request>(string method, string url, value<string[]?> headers, buffer body, keep fn<void(handle<web.request> request)> done)",
         "Start a request with a method, headers (a list of texts such as \"Accept: text/html\") and a body; done, if given, gets the request once it is done\n"
         "@param method The method, such as \"GET\", \"POST\" or \"HEAD\"\n"
         "@param url The URL, http:// or https://; one with another scheme fails\n"
         "@param headers The headers, each a text such as \"Content-Type: application/json\"; nothing for none\n"
         "@param body The body to send, copied; empty for none; a body makes a request POST unless the method names another\n"
         "@param done Called once, on the main thread, when the request is done or failed; its request is the request, in a new handle if the first was dropped; not called if the request is freed first; nothing for no call\n"
         "@return The request, or nothing if it cannot be made"},
        {"status", (ECSFunction)WebStatus, "int(handle<web.request> request)",
         "Give a request's HTTP status once it is done, 0 while it runs, or -1 if it failed\n"
         "@param request The request\n"
         "@return The answer's HTTP status once it is done, such as 200 or 404; 0 while it runs; -1 if it failed"},
        {"body", (ECSFunction)WebBody, "buffer(handle<web.request> request)",
         "Give the body of a request's answer once it is done\n"
         "@param request The request\n"
         "@return The last answer's body, kept while the request lives; empty while it runs"},
        {"header", (ECSFunction)WebHeader, "string(handle<web.request> request, string name)",
         "Give a header of a request's answer once it is done, or nothing\n"
         "@param request The request\n"
         "@param name The header's name, such as \"Content-Type\", in any case\n"
         "@return The last answer's value of the header, kept until the next header on the request; nothing while it runs or if the answer has no such header"},
        {"error", (ECSFunction)WebError, "string(handle<web.request> request)",
         "Give why a request failed\n"
         "@param request The request\n"
         "@return Why it failed; empty while it runs or if it did not fail"},
        {"free", (ECSFunction)WebFreeRequest, "void(handle<web.request> request)",
         "Free a request now; one that runs stops\n"
         "@param request The request; its handle must not be used after, and its done is not called"},
        {"socket", (ECSFunction)WebOpenSocket, "handle<web.socket>(string url, value<string[]?> headers)",
         "Start a WebSocket connection to a ws:// or wss:// URL, with headers (a list of texts such as \"Origin: http://localhost\")\n"
         "@param url The URL, ws:// or wss://\n"
         "@param headers The headers, each a text such as \"Origin: http://localhost\"; nothing for none\n"
         "@return The connection, or nothing if it cannot be made"},
        {"socketStatus", (ECSFunction)WebSocketStatus, "int(handle<web.socket> socket)",
         "Give a connection's state: 1 open, 0 connecting, -1 failed or closed\n"
         "@param socket The connection\n"
         "@return 1 once the server took it; 0 while it connects; -1 if it failed or closed"},
        {"socketSend", (ECSFunction)WebSocketSend, "bool(handle<web.socket> socket, buffer data, bool text)",
         "Send a message, as text or as bytes; it goes once the connection is open\n"
         "@param socket The connection\n"
         "@param data The message, copied; it goes in one frame\n"
         "@param text True: it goes as text; false: as bytes\n"
         "@return False if the connection failed or closed"},
        {"socketReceive", (ECSFunction)WebSocketReceive, "buffer(handle<web.socket> socket, out string type)",
         "Take the oldest message that arrived, and its type, \"text\" or \"binary\"; nothing if none has\n"
         "@param socket The connection\n"
         "@param type The message's type, \"text\" or \"binary\"; nothing if no message waits\n"
         "@return The message, whole, kept until the next receive on the connection; empty if none waits"},
        {"socketError", (ECSFunction)WebSocketError, "string(handle<web.socket> socket)",
         "Give why a connection failed or closed\n"
         "@param socket The connection\n"
         "@return Why it failed or closed; empty while it connects or is open"},
        {"socketClose", (ECSFunction)WebFreeRequest, "void(handle<web.socket> socket)",
         "Close a connection and free it now\n"
         "@param socket The connection; its handle must not be used after"},
    };

    // a plugin whose Init fails is not destroyed, so it cleans up here
    const ECSPluginDesc desc = {.Destroy = WebFree};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc), WebFree(nullptr););
    SHU_ReturnResult(ECSSetting_Declare(plugin, &timeout), WebFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "request", WebRequestDestroy), WebFree(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "socket", WebRequestDestroy), WebFree(nullptr););

    for (usz i = 0; i < (usz)SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), WebFree(nullptr););
    }

    // Lua calls these on a handle, such as handle:status(...); C calls the functions themselves (OpenECS DESIGN 10.6)
    const struct
    {
        const char *type;
        const char *method;
        const char *function;
    } methods[] = {
        {"request", "status", "status"},
        {"request", "body", "body"},
        {"request", "header", "header"},
        {"request", "error", "error"},
        {"request", "free", "free"},
        {"socket", "status", "socketStatus"},
        {"socket", "send", "socketSend"},
        {"socket", "receive", "socketReceive"},
        {"socket", "error", "socketError"},
        {"socket", "close", "socketClose"},
    };

    for (usz i = 0; i < SDL_arraysize(methods); i++)
    {
        SHU_ReturnResult(ECSHandle_AddMethod(plugin, methods[i].type, methods[i].method, methods[i].function), WebFree(nullptr););
    }

    return SHUResult_Ok;
}
