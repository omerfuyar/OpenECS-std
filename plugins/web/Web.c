// The web standard plugin: HTTP and HTTPS requests, and WebSocket connections, that never block the program (DESIGN 11).
// libcurl makes them, with TLS from mbedTLS; both are compiled into this plugin (DESIGN 1). One thread of the plugin runs every request and connection.
// A plugin gives a function that gets the request once it is done, or asks for its state and takes its answer when it wants.
// A WebSocket connection is a request that goes on: the plugin asks for its state, and sends and takes messages when it wants.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#include <curl/curl.h>
#include <mbedtls/platform.h>

#pragma region Source Only

/// @brief The most redirects a request follows.
#define WEB_REDIRECTS 10

/// @brief How often, in seconds, the main thread looks for requests whose function is to be called.
#define WEB_DONE_INTERVAL 0.02

struct WebRequest;

/// @brief A WebSocket message, in a list.
typedef struct WebMessage
{
    u8 *data;
    usz size;
    usz capacity;
    bool text;
    struct WebMessage *next;
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
    u8 *body; // what has arrived
    usz size;
    usz capacity;
    i32 status; // the HTTP status once done, or -1 if it failed
    bool done;
    bool forgotten;  // freed by its owner while it runs; the plugin's thread frees it
    bool running;    // given to the multi handle, on the plugin's thread
    struct WebRequest *next;      // in the queue of new requests
    struct WebRequest *forgottenNext; // in the queue of forgotten requests; apart from next, which the plugin's thread may still read after it took the queue
    WebDoneFunction Done;     // main thread: the function to call once it is done, or nullptr once called
    struct WebRequest *after; // main thread: in the list of every request that is not freed

    // a WebSocket connection's; the lock guards open and the lists, and the rest belongs to the thread that the note names
    bool socket;
    bool open;                    // the server took the connection
    WebMessage *inbox;            // the messages that arrived, oldest first
    WebMessage *outbox;           // the messages to send, oldest first
    bool waking;                  // in the queue of connections with messages to send
    struct WebRequest *wakeNext;  // in that queue
    bool paused;                  // plugin's thread: nothing was left to send, so curl waits until it is woken
    WebMessage *partial;          // plugin's thread: the message that is arriving
    WebMessage *sending;          // plugin's thread: the message that is going out
    usz sent;                     // plugin's thread: how much of it went
    WebMessage *taken;            // main thread: the message that the last receive gave, kept until the next one
} WebRequest;

static struct
{
    ECSPlugin plugin;
    bool curl;           // curl_global_init worked
    CURLM *multi;        // used only on the plugin's thread, except to wake it
    SDL_Thread *thread;  // started by the first request
    SDL_Mutex *lock;
    WebRequest *added;     // waiting for the plugin's thread
    WebRequest *forgotten; // running, freed by their owner
    WebRequest *wake;      // connections with messages to send
    bool quit;
    const char *certificates; // the system's file of trusted certificates, or nullptr
    WebRequest *all;          // main thread: every request that is not freed
    ECSTimer timer;           // main thread: calls the functions of the requests that are done, while some wait
} WEB = {0};

/// @brief Keeps what arrives, on the plugin's thread.
static size_t WebWrite(char *data, size_t size, size_t count, void *user)
{
    WebRequest *request = user;
    usz length = size * count;

    if (request->size + length > request->capacity)
    {
        usz capacity = SDL_max(request->capacity * 2, request->size + length);
        u8 *body = SDL_realloc(request->body, capacity);

        if (body == nullptr)
        {
            return CURL_WRITEFUNC_ERROR;
        }

        request->body = body;
        request->capacity = capacity;
    }

    SDL_memcpy(request->body + request->size, data, length);
    request->size += length;
    return length;
}

/// @brief Frees a list of messages.
static void WebFreeMessages(WebMessage *message)
{
    while (message != nullptr)
    {
        WebMessage *next = message->next;
        SDL_free(message->data);
        SDL_free(message);
        message = next;
    }
}

/// @brief Adds a message at the end of a list.
static void WebAppend(WebMessage **list, WebMessage *message)
{
    while (*list != nullptr)
    {
        list = &(*list)->next;
    }

    *list = message;
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

    WebMessage *message = request->partial;

    if (message == nullptr)
    {
        message = SDL_calloc(1, sizeof(WebMessage));

        if (message == nullptr)
        {
            return CURL_WRITEFUNC_ERROR;
        }

        message->text = (frame->flags & CURLWS_TEXT) != 0;
        request->partial = message;
    }

    if (message->size + length > message->capacity)
    {
        usz capacity = SDL_max(message->capacity * 2, message->size + length);
        u8 *bytes = SDL_realloc(message->data, capacity);

        if (bytes == nullptr)
        {
            return CURL_WRITEFUNC_ERROR;
        }

        message->data = bytes;
        message->capacity = capacity;
    }

    SDL_memcpy(message->data + message->size, data, length);
    message->size += length;

    // the last piece of the last frame of a message ends it; CURLWS_CONT marks every frame but the last
    if (frame->bytesleft == 0 && (frame->flags & CURLWS_CONT) == 0)
    {
        request->partial = nullptr;
        SDL_LockMutex(WEB.lock);
        WebAppend(&request->inbox, message);
        SDL_UnlockMutex(WEB.lock);
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

    if (request->sending == nullptr && request->outbox != nullptr)
    {
        request->sending = request->outbox;
        request->outbox = request->outbox->next;
        request->sending->next = nullptr;
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
    if (request->sent == 0 && curl_ws_start_frame(request->easy, message->text ? CURLWS_TEXT : CURLWS_BINARY, (curl_off_t)message->size) != CURLE_OK)
    {
        return CURL_READFUNC_ABORT;
    }

    usz length = SDL_min(room, message->size - request->sent);
    SDL_memcpy(buffer, message->data + request->sent, length);
    request->sent += length;

    if (request->sent == message->size)
    {
        WebFreeMessages(message);
        request->sending = nullptr;
    }

    return length;
}

/// @brief Frees a request's memory, which no thread uses any longer.
static void WebFreeMemory(WebRequest *request)
{
    WebFreeMessages(request->inbox);
    WebFreeMessages(request->outbox);
    WebFreeMessages(request->partial);
    WebFreeMessages(request->sending);
    WebFreeMessages(request->taken);
    curl_easy_cleanup(request->easy);
    curl_slist_free_all(request->headers);
    SDL_free(request->body);
    SDL_free(request);
}

/// @brief Frees a request on the main thread: it leaves the list, its function is released without a call, and one that runs stops.
static void WebDelete(WebRequest *request)
{
    for (WebRequest **link = &WEB.all; *link != nullptr; link = &(*link)->after)
    {
        if (*link == request)
        {
            *link = request->after;
            break;
        }
    }

    ECSService_ReleaseCallback((ECSFunction)request->Done);
    request->Done = nullptr;

    // a request that runs is freed by the plugin's thread, which is the one that may touch it now
    SDL_LockMutex(WEB.lock);
    bool running = request->running && !request->done;

    if (running)
    {
        request->forgotten = true;
        request->forgottenNext = WEB.forgotten;
        WEB.forgotten = request;
    }

    // a request that waits in the queue leaves it, and so does a connection that waits to be woken
    for (WebRequest **link = &WEB.added; *link != nullptr; link = &(*link)->next)
    {
        if (*link == request)
        {
            *link = request->next;
            break;
        }
    }

    for (WebRequest **link = &WEB.wake; *link != nullptr; link = &(*link)->wakeNext)
    {
        if (*link == request)
        {
            *link = request->wakeNext;
            break;
        }
    }

    SDL_UnlockMutex(WEB.lock);

    if (running)
    {
        curl_multi_wakeup(WEB.multi);
        return;
    }

    WebFreeMemory(request);
}

/// @brief The destructor of a request's handle. A request whose function waits goes on, and its function gets it in a new handle.
static void WebDestroy(void *object)
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
    WebRequest *added = WEB.added;
    WebRequest *forgotten = WEB.forgotten;
    WebRequest *wake = WEB.wake;
    bool quit = WEB.quit;
    WEB.added = nullptr;
    WEB.forgotten = nullptr;
    WEB.wake = nullptr;

    for (WebRequest *request = added; request != nullptr; request = request->next)
    {
        request->running = true;
    }

    // a connection is woken only by the thread that runs it; one that is not paused sends its messages anyway, and one that ended has nothing to wake
    // the lock stays held, so the main thread cannot free one of them meanwhile; SDL's mutexes are recursive, so curl's callbacks still take it
    for (WebRequest *request = wake; request != nullptr; request = request->wakeNext)
    {
        request->waking = false;

        if (request->paused && request->running && !request->done)
        {
            request->paused = false;
            curl_easy_pause(request->easy, CURLPAUSE_CONT);
        }
    }

    SDL_UnlockMutex(WEB.lock);

    while (added != nullptr)
    {
        WebRequest *next = added->next;

        if (curl_multi_add_handle(WEB.multi, added->easy) != CURLM_OK)
        {
            SDL_LockMutex(WEB.lock);
            SDL_strlcpy(added->error, "The request cannot start", sizeof(added->error));
            added->status = -1;
            added->done = true;
            SDL_UnlockMutex(WEB.lock);
        }

        added = next;
    }

    while (forgotten != nullptr)
    {
        WebRequest *next = forgotten->forgottenNext;
        curl_multi_remove_handle(WEB.multi, forgotten->easy);
        WebFreeMemory(forgotten);
        forgotten = next;
    }

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
static bool WebStart(void)
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

    ECS_LogWarn(WEB.plugin, "No file of trusted certificates is found, so HTTPS requests fail; SSL_CERT_FILE can name one");
    return nullptr;
}

/// @brief Sets a request's method and body: a body makes it a POST, unless the method names another.
static bool WebSetMethod(CURL *easy, const char *method, SHUSlice body)
{
    bool ok = true;

    if (body.size > 0 || SDL_strcasecmp(method, "POST") == 0)
    {
        ok = curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size) == CURLE_OK &&
             curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, body.size > 0 ? (const char *)body.data : "") == CURLE_OK;
    }

    if (SDL_strcasecmp(method, "HEAD") == 0)
    {
        return ok && curl_easy_setopt(easy, CURLOPT_NOBODY, 1L) == CURLE_OK;
    }

    // curl sends a body with POST, so a GET with a body names its method
    if ((SDL_strcasecmp(method, "GET") == 0 && body.size == 0) || SDL_strcasecmp(method, "POST") == 0)
    {
        return ok;
    }

    return ok && curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method) == CURLE_OK;
}

static bool WebDone(WebRequest *request);

/// @brief Calls the functions of the requests that are done, one at a time, because a function may free requests.
static void WebCallDone(void *data)
{
    (void)data;
    bool waiting = true;

    while (waiting)
    {
        WebRequest *request = WEB.all;
        waiting = false;

        while (request != nullptr && (request->Done == nullptr || !WebDone(request)))
        {
            waiting = waiting || request->Done != nullptr;
            request = request->after;
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

static WebRequest *WebRequestNew(const char *method, const char *url, const ECSValue *headers, SHUSlice body, WebDoneFunction Done)
{
    WebRequest *request = SDL_calloc(1, sizeof(WebRequest));
    CURL *easy = request == nullptr ? nullptr : curl_easy_init();

    if (easy == nullptr || !WebStart())
    {
        SDL_free(request);
        curl_easy_cleanup(easy);
        ECSService_ReleaseCallback((ECSFunction)Done);
        return nullptr;
    }

    request->easy = easy;
    f64 timeout = ECSValue_GetNumber(ECSSetting_Get("web.timeout"), 30.0);

    // the headers are a list of texts, such as "Accept: text/html"
    bool ok = true;

    for (usz i = 0; ok && i < ECSValue_GetListCount(headers); i++)
    {
        struct curl_slist *list = curl_slist_append(request->headers, ECSValue_GetString(ECSValue_GetListItem(headers, i), ""));
        ok = list != nullptr;
        request->headers = ok ? list : request->headers;
    }

    ok = ok && curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_PRIVATE, request) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, request->error) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, WebWrite) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_WRITEDATA, request) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_MAXREDIRS, (long)WEB_REDIRECTS) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https") == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https") == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_USERAGENT, "OpenECS/" OPENECS_VERSION) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, (long)(SDL_max(timeout, 0.0) * 1000.0)) == CURLE_OK &&
              (WEB.certificates == nullptr || curl_easy_setopt(easy, CURLOPT_CAINFO, WEB.certificates) == CURLE_OK) &&
              WebSetMethod(easy, method, body);

    ok = ok && curl_easy_setopt(easy, CURLOPT_HTTPHEADER, request->headers) == CURLE_OK;

    if (Done != nullptr && WEB.timer == nullptr)
    {
        ok = ok && ECSTimer_Start(WEB.plugin, &WEB.timer, WEB_DONE_INTERVAL, true, WebCallDone, nullptr) == SHUResult_Ok;
    }

    if (!ok)
    {
        ECS_LogWarn(WEB.plugin, "Cannot make a request to '%s'", url);
        WebFreeMemory(request);
        ECSService_ReleaseCallback((ECSFunction)Done);
        return nullptr;
    }

    request->Done = Done;
    request->after = WEB.all;
    WEB.all = request;

    SDL_LockMutex(WEB.lock);
    request->next = WEB.added;
    WEB.added = request;
    SDL_UnlockMutex(WEB.lock);
    curl_multi_wakeup(WEB.multi);
    return request;
}

static WebRequest *WebGet(const char *url, WebDoneFunction Done)
{
    return WebRequestNew("GET", url, nullptr, (SHUSlice){0}, Done);
}

static i32 WebStatus(WebRequest *request)
{
    SDL_LockMutex(WEB.lock);
    i32 status = request->done ? request->status : 0;
    SDL_UnlockMutex(WEB.lock);
    return status;
}

static bool WebDone(WebRequest *request)
{
    return WebStatus(request) != 0;
}

static SHUSlice WebBody(WebRequest *request)
{
    return WebDone(request) ? (SHUSlice){.data = request->body, .size = request->size} : (SHUSlice){0};
}

static const char *WebHeader(WebRequest *request, const char *name)
{
    struct curl_header *header = nullptr;

    // -1 is the last response, after the redirects
    if (!WebDone(request) || curl_easy_header(request->easy, name, 0, CURLH_HEADER, -1, &header) != CURLHE_OK)
    {
        return nullptr;
    }

    return header->value;
}

static const char *WebError(WebRequest *request)
{
    return WebDone(request) ? request->error : "";
}

/// @brief Frees a request now, instead of when no one uses its handle; one that runs stops, and its function is not called.
static void WebFree(WebRequest *request)
{
    ECSHandle_Forget(request);
    WebDelete(request);
}

/// @brief Starts a WebSocket connection to a ws:// or wss:// URL, with headers, a list of texts.
static WebRequest *WebSocketOpen(const char *url, const ECSValue *headers)
{
    WebRequest *request = SDL_calloc(1, sizeof(WebRequest));
    CURL *easy = request == nullptr ? nullptr : curl_easy_init();

    if (easy == nullptr || !WebStart())
    {
        SDL_free(request);
        curl_easy_cleanup(easy);
        return nullptr;
    }

    request->easy = easy;
    request->socket = true;
    f64 timeout = ECSValue_GetNumber(ECSSetting_Get("web.timeout"), 30.0);
    bool ok = true;

    for (usz i = 0; ok && i < ECSValue_GetListCount(headers); i++)
    {
        struct curl_slist *list = curl_slist_append(request->headers, ECSValue_GetString(ECSValue_GetListItem(headers, i), ""));
        ok = list != nullptr;
        request->headers = ok ? list : request->headers;
    }

    // the connection goes on until one side closes it, so the timeout limits only how long it takes to connect;
    // curl asks the read function for messages to send, as for an upload
    ok = ok && curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_PRIVATE, request) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, request->error) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, WebSocketWrite) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_WRITEDATA, request) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_READFUNCTION, WebSocketRead) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_READDATA, request) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_UPLOAD, 1L) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "ws,wss") == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_USERAGENT, "OpenECS/" OPENECS_VERSION) == CURLE_OK &&
              curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, (long)(SDL_max(timeout, 0.0) * 1000.0)) == CURLE_OK &&
              (WEB.certificates == nullptr || curl_easy_setopt(easy, CURLOPT_CAINFO, WEB.certificates) == CURLE_OK) &&
              curl_easy_setopt(easy, CURLOPT_HTTPHEADER, request->headers) == CURLE_OK;

    if (!ok)
    {
        ECS_LogWarn(WEB.plugin, "Cannot connect to '%s'", url);
        WebFreeMemory(request);
        return nullptr;
    }

    request->after = WEB.all;
    WEB.all = request;

    SDL_LockMutex(WEB.lock);
    request->next = WEB.added;
    WEB.added = request;
    SDL_UnlockMutex(WEB.lock);
    curl_multi_wakeup(WEB.multi);
    return request;
}

static i32 WebSocketStatus(WebRequest *socket)
{
    SDL_LockMutex(WEB.lock);
    i32 status = socket->done ? -1 : socket->open ? 1 : 0;
    SDL_UnlockMutex(WEB.lock);
    return status;
}

/// @brief Queues a message to send, as text or as bytes. It goes once the connection is open.
/// @return false if the connection failed or closed, or memory runs out.
static bool WebSocketSend(WebRequest *socket, SHUSlice data, bool text)
{
    WebMessage *message = SDL_calloc(1, sizeof(WebMessage));
    u8 *bytes = message == nullptr ? nullptr : SDL_malloc(SDL_max(data.size, (usz)1));

    if (bytes == nullptr || WebSocketStatus(socket) < 0)
    {
        SDL_free(message);
        SDL_free(bytes);
        return false;
    }

    SDL_memcpy(bytes, data.data, data.size);
    *message = (WebMessage){.data = bytes, .size = data.size, .capacity = data.size, .text = text};

    // the plugin's thread wakes the connection, since curl wants it woken from the thread that runs it
    SDL_LockMutex(WEB.lock);
    WebAppend(&socket->outbox, message);

    if (!socket->waking)
    {
        socket->waking = true;
        socket->wakeNext = WEB.wake;
        WEB.wake = socket;
    }

    SDL_UnlockMutex(WEB.lock);
    curl_multi_wakeup(WEB.multi);
    return true;
}

/// @brief Takes the oldest message that arrived, and whether it is "text" or "binary"; nothing if none waits. The message is kept until the next receive.
static SHUSlice WebSocketReceive(WebRequest *socket, const char **retKind)
{
    WebFreeMessages(socket->taken);

    SDL_LockMutex(WEB.lock);
    WebMessage *message = socket->inbox;
    socket->inbox = message == nullptr ? nullptr : message->next;
    SDL_UnlockMutex(WEB.lock);

    socket->taken = message;

    if (message == nullptr)
    {
        *retKind = nullptr;
        return (SHUSlice){0};
    }

    message->next = nullptr;
    *retKind = message->text ? "text" : "binary";
    return (SHUSlice){.data = message->data, .size = message->size};
}

static const char *WebSocketError(WebRequest *socket)
{
    return WebSocketStatus(socket) < 0 ? socket->error : "";
}

static void WebQuit(void *data)
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

    while (WEB.all != nullptr)
    {
        WebRequest *request = WEB.all;
        WEB.all = request->after;
        ECSService_ReleaseCallback((ECSFunction)request->Done);
        curl_multi_remove_handle(WEB.multi, request->easy);
        WebFreeMemory(request);
    }

    curl_multi_cleanup(WEB.multi);

    if (WEB.curl)
    {
        curl_global_cleanup();
    }

    SDL_DestroyMutex(WEB.lock);
    SDL_zero(WEB);
}

#pragma endregion Source Only

SHUResult ECSPlugin_Init(ECSPlugin plugin)
{
    WEB.plugin = plugin;

    // curl and mbedTLS take memory from SDL's allocator, as the core does
    mbedtls_platform_set_calloc_free(SDL_calloc, SDL_free);
    WEB.curl = curl_global_init_mem(CURL_GLOBAL_DEFAULT, SDL_malloc, SDL_free, SDL_realloc, SDL_strdup, SDL_calloc) == CURLE_OK;
    WEB.multi = WEB.curl ? curl_multi_init() : nullptr;
    WEB.lock = SDL_CreateMutex();

    if (WEB.multi == nullptr || WEB.lock == nullptr)
    {
        ECS_LogError(plugin, "Cannot start libcurl");
        WebQuit(nullptr);
        return SHUResult_ErrInternal;
    }

    WEB.certificates = WebFindCertificates();

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
        {"get", (ECSFunction)WebGet, "handle<web.request>(string url, keep fn<void(handle<web.request> request)> done)", "Start a GET request to a URL; done, if given, gets the request once it is done"},
        {"request", (ECSFunction)WebRequestNew, "handle<web.request>(string method, string url, value<string[]?> headers, buffer body, keep fn<void(handle<web.request> request)> done)", "Start a request with a method, headers (a list of texts such as \"Accept: text/html\") and a body; done, if given, gets the request once it is done"},
        {"status", (ECSFunction)WebStatus, "int(handle<web.request> request)", "Give a request's HTTP status once it is done, 0 while it runs, or -1 if it failed"},
        {"body", (ECSFunction)WebBody, "buffer(handle<web.request> request)", "Give the body of a request's answer once it is done"},
        {"header", (ECSFunction)WebHeader, "string(handle<web.request> request, string name)", "Give a header of a request's answer once it is done, or nothing"},
        {"error", (ECSFunction)WebError, "string(handle<web.request> request)", "Give why a request failed"},
        {"free", (ECSFunction)WebFree, "void(handle<web.request> request)", "Free a request now; one that runs stops"},
        {"socket", (ECSFunction)WebSocketOpen, "handle<web.socket>(string url, value<string[]?> headers)", "Start a WebSocket connection to a ws:// or wss:// URL, with headers (a list of texts such as \"Origin: http://localhost\")"},
        {"socketStatus", (ECSFunction)WebSocketStatus, "int(handle<web.socket> socket)", "Give a connection's state: 1 open, 0 connecting, -1 failed or closed"},
        {"socketSend", (ECSFunction)WebSocketSend, "bool(handle<web.socket> socket, buffer data, bool text)", "Send a message, as text or as bytes; it goes once the connection is open"},
        {"socketReceive", (ECSFunction)WebSocketReceive, "buffer(handle<web.socket> socket, out string kind)", "Take the oldest message that arrived, and its kind, \"text\" or \"binary\"; nothing if none has"},
        {"socketError", (ECSFunction)WebSocketError, "string(handle<web.socket> socket)", "Give why a connection failed or closed"},
        {"socketClose", (ECSFunction)WebFree, "void(handle<web.socket> socket)", "Close a connection and free it now"},
    };

    // a plugin whose Init fails is not destroyed, so it cleans up here
    const ECSPluginDesc desc = {.Destroy = WebQuit};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc), WebQuit(nullptr););
    SHU_ReturnResult(ECSSetting_Declare(plugin, &timeout), WebQuit(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "request", WebDestroy), WebQuit(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "socket", WebDestroy), WebQuit(nullptr););

    for (usz i = 0; i < (usz)SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), WebQuit(nullptr););
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
        SHU_ReturnResult(ECSHandle_AddMethod(plugin, methods[i].type, methods[i].method, methods[i].function), WebQuit(nullptr););
    }

    return SHUResult_Ok;
}
