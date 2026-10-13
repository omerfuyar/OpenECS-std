// The web standard plugin: HTTP and HTTPS requests that never block the program (DESIGN 11).
// libcurl makes the requests, with TLS from mbedTLS; both are compiled into this plugin (DESIGN 1). One thread of the plugin runs every request,
// and a plugin asks for a request's state and takes its answer when it wants, such as from a timer.

#include "OpenECS.h"

#include "SDL3/SDL.h"

#include <curl/curl.h>
#include <mbedtls/platform.h>

#pragma region Source Only

/// @brief The most redirects a request follows.
#define WEB_REDIRECTS 10

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
    struct WebRequest *next; // in the queue of new or of forgotten requests
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
    bool quit;
    const char *certificates; // the system's file of trusted certificates, or nullptr
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

static void WebDestroy(void *object)
{
    WebRequest *request = object;

    // a request that runs is freed by the plugin's thread, which is the one that may touch it now
    SDL_LockMutex(WEB.lock);
    bool running = request->running && !request->done;

    if (running)
    {
        request->forgotten = true;
        request->next = WEB.forgotten;
        WEB.forgotten = request;
    }

    // a request that waits in the queue leaves it
    for (WebRequest **link = &WEB.added; *link != nullptr; link = &(*link)->next)
    {
        if (*link == request)
        {
            *link = request->next;
            break;
        }
    }

    SDL_UnlockMutex(WEB.lock);

    if (running)
    {
        curl_multi_wakeup(WEB.multi);
        return;
    }

    curl_easy_cleanup(request->easy);
    curl_slist_free_all(request->headers);
    SDL_free(request->body);
    SDL_free(request);
}

/// @brief Takes the requests that are queued and forgotten, on the plugin's thread.
/// @return false once the plugin quits.
static bool WebTakeQueues(void)
{
    SDL_LockMutex(WEB.lock);
    WebRequest *added = WEB.added;
    WebRequest *forgotten = WEB.forgotten;
    bool quit = WEB.quit;
    WEB.added = nullptr;
    WEB.forgotten = nullptr;

    for (WebRequest *request = added; request != nullptr; request = request->next)
    {
        request->running = true;
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
        WebRequest *next = forgotten->next;
        curl_multi_remove_handle(WEB.multi, forgotten->easy);
        forgotten->running = false;
        WebDestroy(forgotten);
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

            request->status = result == CURLE_OK ? (i32)status : -1;
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

    if (SDL_strcasecmp(method, "GET") == 0 || SDL_strcasecmp(method, "POST") == 0)
    {
        return ok;
    }

    return ok && curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method) == CURLE_OK;
}

static WebRequest *WebRequestNew(const char *method, const char *url, const ECSValue *headers, SHUSlice body)
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
    f64 timeout = ECSValue_GetNumber(ECSSetting_Get("web.timeout"), 30.0);

    // the headers are a list of texts, such as "Accept: text/html"
    bool ok = true;

    for (usz i = 0; ok && i < ECSValue_GetListCount(headers); i++)
    {
        struct curl_slist *list = curl_slist_append(request->headers, ECSValue_GetString(ECSValue_GetListItem(headers, i), ""));
        ok = list != nullptr;
        request->headers = ok ? list : request->headers;
    }

    ok = ok &&curl_easy_setopt(easy, CURLOPT_URL, url) == CURLE_OK &&
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

    if (!ok)
    {
        ECS_LogWarn(WEB.plugin, "Cannot make a request to '%s'", url);
        WebDestroy(request);
        return nullptr;
    }

    SDL_LockMutex(WEB.lock);
    request->next = WEB.added;
    WEB.added = request;
    SDL_UnlockMutex(WEB.lock);
    curl_multi_wakeup(WEB.multi);
    return request;
}

static WebRequest *WebGet(const char *url)
{
    return WebRequestNew("GET", url, nullptr, (SHUSlice){0});
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

/// @brief Frees a request now, instead of when no one uses its handle; one that runs stops.
static void WebFree(WebRequest *request)
{
    ECSHandle_Forget(request);
    WebDestroy(request);
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

    // the handles are destroyed before the plugins, so no request is left but those that C plugins keep
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
        {"get", (ECSFunction)WebGet, "handle<web.request>(string url)", "Start a GET request to a URL"},
        {"request", (ECSFunction)WebRequestNew, "handle<web.request>(string method, string url, value headers, buffer body)", "Start a request with a method, headers (a list of texts such as \"Accept: text/html\") and a body"},
        {"status", (ECSFunction)WebStatus, "int(handle<web.request> request)", "Give a request's HTTP status once it is done, 0 while it runs, or -1 if it failed"},
        {"body", (ECSFunction)WebBody, "buffer(handle<web.request> request)", "Give the body of a request's answer once it is done"},
        {"header", (ECSFunction)WebHeader, "string(handle<web.request> request, string name)", "Give a header of a request's answer once it is done, or nothing"},
        {"error", (ECSFunction)WebError, "string(handle<web.request> request)", "Give why a request failed"},
        {"free", (ECSFunction)WebFree, "void(handle<web.request> request)", "Free a request now; one that runs stops"},
    };

    // a plugin whose Init fails is not destroyed, so it cleans up here
    const ECSPluginDesc desc = {.Destroy = WebQuit};
    SHU_ReturnResult(ECSPlugin_Register(plugin, &desc), WebQuit(nullptr););
    SHU_ReturnResult(ECSSetting_Declare(plugin, &timeout), WebQuit(nullptr););
    SHU_ReturnResult(ECSHandle_RegisterType(plugin, "request", WebDestroy), WebQuit(nullptr););

    for (usz i = 0; i < (usz)SDL_arraysize(functions); i++)
    {
        SHU_ReturnResult(ECSService_RegisterFunction(plugin, functions[i].name, functions[i].function, functions[i].signature, functions[i].description), WebQuit(nullptr););
    }

    return SHUResult_Ok;
}
