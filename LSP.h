#ifndef _H_LSP
#define _H_LSP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Platform detection ──────────────────────────────────────────────────── */
#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <windows.h>
#   include <io.h>

    /* thin POSIX-like wrappers so the rest of the code stays readable */
    typedef HANDLE      lsp_thread_t;
    typedef HANDLE      lsp_pipe_fd_t;
#   define LSP_INVALID_FD  INVALID_HANDLE_VALUE

    static int lsp_pipe_read(lsp_pipe_fd_t fd, void *buf, int n) {
        DWORD got = 0;
        return ReadFile(fd, buf, (DWORD)n, &got, NULL) ? (int)got : -1;
    }
    static int lsp_pipe_write(lsp_pipe_fd_t fd, const void *buf, int n) {
        DWORD wrote = 0;
        return WriteFile(fd, buf, (DWORD)n, &wrote, NULL) ? (int)wrote : -1;
    }
    static void lsp_pipe_close(lsp_pipe_fd_t fd) { CloseHandle(fd); }
    static void lsp_sleep_us(int us)              { Sleep((DWORD)((us + 999) / 1000)); }

#else  /* POSIX */
#   include <unistd.h>
#   include <sys/wait.h>
#   include <pthread.h>

    typedef pthread_t   lsp_thread_t;
    typedef int         lsp_pipe_fd_t;
#   define LSP_INVALID_FD  (-1)

    static int lsp_pipe_read(lsp_pipe_fd_t fd, void *buf, int n)        { return (int)read(fd, buf, (size_t)n); }
    static int lsp_pipe_write(lsp_pipe_fd_t fd, const void *buf, int n) { return (int)write(fd, buf, (size_t)n); }
    static void lsp_pipe_close(lsp_pipe_fd_t fd)                        { close(fd); }
    static void lsp_sleep_us(int us)                                     { usleep((useconds_t)us); }
#endif

/* ── Optional dependency guards ─────────────────────────────────────────── */
#ifndef LSP_NO_STB_DS
#   define STB_DS_IMPLEMENTATION
#   include "stb_ds.h"
#endif

#ifndef LSP_NO_CJSON
#   define CJSON_NO_STB_DS
#   include "parser.h"
#endif

#ifndef LSP_NO_TINY_QUEUE
#   include "tiny_queue.h"
#endif

/* ── Public types ────────────────────────────────────────────────────────── */
typedef struct {
    char* name;
    char* cmd;
} RegisteredLSP;

typedef struct {
    char* key;
    RegisteredLSP* value;
} LSPIDTable;

typedef int32_t LSPID;

static LSPIDTable* lsp_id_table = NULL;
static int32_t lsp_id_iota = 1;


static int lsp_command_exists(const char *cmd) {
#ifdef _WIN32
    /* Use 'where' on Windows instead of 'which' */
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "where \"%s\" > NUL 2>&1", cmd);
#else
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "which %s > /dev/null 2>&1", cmd);
#endif
    return system(buffer) == 0;
}

void clear_lsp_registry(){
    lsp_id_iota = 1;
    if(lsp_id_table){
        for(size_t i = 0; i < shlen(lsp_id_table); i++){
            LSPIDTable entry = lsp_id_table[i];
            free(entry.value->name);
            free(entry.value->cmd);
            free(entry.value);
        }
        shfree(lsp_id_table);
        lsp_id_table = NULL;
    }
}

LSPID register_lsp(const char* name, const char* cmd){
    if(lsp_id_table){
        for(size_t i = 0; i < shlen(lsp_id_table); i++){
            LSPIDTable entry = lsp_id_table[i];
            if(strncmp(name, entry.key, strlen(name)) == 0){
                // LSP With that name already exists
                return -1;
            }
        }
    }
    if(!lsp_command_exists(cmd)){
        // LSP is not installed
        return -1;
    }
    RegisteredLSP* reg = malloc(sizeof(RegisteredLSP));
    if(!reg){
        return -1;
    }
    reg->name = strdup(name);
    reg->cmd = strdup(cmd);
    shput(lsp_id_table, reg->name, reg);
    return lsp_id_iota++;
}


void destroy_registered_lsps(){
    for(size_t i = 0; i < shlen(lsp_id_table); i++){
        LSPIDTable entry = lsp_id_table[i];
        if(entry.value){
            if(entry.value->name){
                free(entry.value->name);
            }
            if(entry.value->cmd){
                free(entry.value->cmd);
            }
        }
    }
    shfree(lsp_id_table);
    lsp_id_table = NULL;
}

typedef struct {
    bool           running;
    lsp_thread_t   sender_thread;
    tiny_queue_t  *sender_queue;
    lsp_thread_t   receiver_thread;
    tiny_queue_t  *receiver_queue;
    JsonValue     *capabilities;

    lsp_pipe_fd_t  write_fd;   /* parent → server  */
    lsp_pipe_fd_t  read_fd;    /* server → parent  */
    LSPID        kind;
} LSPContext;

/* ── Timeout (µs on POSIX, rounded up to ms on Windows) ─────────────────── */
#ifndef LSP_WAIT_FOR_TIMEOUT_US
#   define LSP_WAIT_FOR_TIMEOUT_US 10
#endif

/* ── Message helpers ─────────────────────────────────────────────────────── */

char* LSP_str_to_arr(const char* str){
    char* arr = NULL;
    for(int i = 0; i < strlen(str); i++){
        arrput(arr, str[i]);
    }
    arrput(arr, '\0');
    return arr;
}

char *get_file_uri(const char *full_path) {
    char *uri = NULL;
    const char prefix[] = "file://";
    for (size_t i = 0; i < sizeof(prefix) - 1; i++) arrput(uri, prefix[i]);
    for (size_t i = 0; i < strlen(full_path); i++)   arrput(uri, full_path[i]);
    arrput(uri, '\0');
    char* escaped = json_escape(uri);
    arrfree(uri);
    char* new_uri = LSP_str_to_arr(escaped);
    free(escaped);
    return new_uri;
}

JsonValue *lsp_wait_for(tiny_queue_t *queue, int id, const char *str_id) {
    while (1) {
        JsonValue *response = tiny_queue_pop(queue);
        if (!response) return NULL;   /* sentinel / EOF */

        JsonValue *response_id = shget(response->object, "id");
        if (response_id && response_id->type == JSON_NUMBER && str_id == NULL &&
                (json_get_integer(response_id)) == id)
            return response;
        if (response_id && response_id->type == JSON_STRING && str_id != NULL &&
                strncmp(response_id->string, str_id, strlen(response_id->string)) == 0)
            return response;

        tiny_queue_push(queue, response);
        lsp_sleep_us(LSP_WAIT_FOR_TIMEOUT_US);
    }
}

static JsonValue *make_base_message(void) {
    JsonValue *msg = json_new_object();
    json_add_child(msg, "jsonrpc", json_new_string("2.0"));
    return msg;
}

#define MAKE_ID(id, str_id) ((str_id) == NULL ? json_new_integer(id) : json_new_string(str_id))

JsonValue *make_initialize_message(double id, const char *str_id, JsonValue *params) {
    JsonValue *msg = make_base_message();
    json_add_child(msg, "id",     MAKE_ID(id, str_id));
    json_add_child(msg, "method", json_new_string("initialize"));
    json_add_child(msg, "params", params ? params : json_new_object());
    return msg;
}

JsonValue *make_initialized_notification(void) {
    JsonValue *msg = make_base_message();
    json_add_child(msg, "method", json_new_string("initialized"));
    json_add_child(msg, "params", json_new_object());
    return msg;
}

JsonValue *make_didOpen_params(const char *document_uri, const char *language_id,
                               int version, const char *text) {
    JsonValue *params        = json_new_object();
    JsonValue *text_document = json_new_object();
    json_add_child(text_document, "uri",        json_new_string(document_uri));
    json_add_child(text_document, "languageId", json_new_string(language_id));
    json_add_child(text_document, "version",    json_new_integer(version));
    json_add_child(text_document, "text",       json_new_string(text));
    json_add_child(params, "textDocument", text_document);
    return params;
}

JsonValue *make_didOpen_notification(JsonValue *params) {
    if (!params) return NULL;
    JsonValue *msg = make_base_message();
    json_add_child(msg, "method", json_new_string("textDocument/didOpen"));
    json_add_child(msg, "params", params);
    return msg;
}

JsonValue *make_didChange_params(const char *document_uri, int version, const char *text) {
    JsonValue *params          = json_new_object();
    JsonValue *text_document   = json_new_object();
    JsonValue *content_changes = json_new_array();
    JsonValue *change          = json_new_object();

    json_add_child(text_document, "uri",     json_new_string(document_uri));
    json_add_child(text_document, "version", json_new_integer(version));
    json_add_child(params, "textDocument", text_document);

    json_add_child(change, "text", json_new_string(text));
    json_add_child(content_changes, NULL, change);
    json_add_child(params, "contentChanges", content_changes);
    return params;
}

JsonValue *make_didChange_notification(JsonValue *params) {
    if (!params) return NULL;
    JsonValue *msg = make_base_message();
    json_add_child(msg, "method", json_new_string("textDocument/didChange"));
    json_add_child(msg, "params", params);
    return msg;
}

JsonValue *make_completion_params(const char *document_uri,
                                  unsigned int line, unsigned int character) {
    JsonValue *params       = json_new_object();
    JsonValue *textDocument = json_new_object();
    JsonValue *position     = json_new_object();
    JsonValue *context      = json_new_object();

    json_add_child(textDocument, "uri", json_new_string(document_uri));
    json_add_child(params, "textDocument", textDocument);

    json_add_child(position, "line",      json_new_integer(line));
    json_add_child(position, "character", json_new_integer(character));
    json_add_child(params, "position", position);

    json_add_child(params, "workDoneToken", json_new_null());

    json_add_child(context, "triggerKind",      json_new_integer(1));
    json_add_child(context, "triggerCharacter", json_new_null());
    json_add_child(params, "context", context);
    return params;
}

JsonValue *make_completion_request(JsonValue *id, JsonValue *params) {
    JsonValue *msg = make_base_message();
    json_add_child(msg, "id",     id);
    json_add_child(msg, "method", json_new_string("textDocument/completion"));
    json_add_child(msg, "params", params);
    return msg;
}

/* ── Wire-level I/O ──────────────────────────────────────────────────────── */

void send_json_rpc_message(lsp_pipe_fd_t fd, JsonValue *packet) {
    char *out = NULL;
    json_dump(packet, &out, 0, 0);
    arrput(out, '\0');

    int   len    = (int)strlen(out);
    int   bufsz  = len + 64;
    char *buffer = (char *)malloc((size_t)bufsz);
    int   n      = snprintf(buffer, (size_t)bufsz, "Content-Length: %d\r\n\r\n%s", len, out);
    lsp_pipe_write(fd, buffer, n);
    arrfree(out);
    free(buffer);
}

static JsonValue *receive_json_rpc_message(lsp_pipe_fd_t fd) {
    char header[256] = {0};
    int  pos         = 0;

    /* Read until we see the blank line separating header from body */
    while (1) {
        if (lsp_pipe_read(fd, &header[pos], 1) <= 0) return NULL;
        pos++;
        if (pos >= 4 && memcmp(header + pos - 4, "\r\n\r\n", 4) == 0) break;
        if (pos >= (int)sizeof(header) - 1) return NULL;
    }

    int content_length = 0;
    sscanf(header, "Content-Length: %d", &content_length);
    if (content_length <= 0) return NULL;

    char *body  = (char *)malloc((size_t)(content_length + 1));
    int   total = 0;
    while (total < content_length) {
        int n = lsp_pipe_read(fd, body + total, content_length - total);
        if (n <= 0) { free(body); return NULL; }
        total += n;
    }
    body[content_length] = '\0';

    JsonValue *response = json_new_object();
    jsonStringLoad(body, response);
    free(body);
    return response;
}

/* ── Thread entry points ─────────────────────────────────────────────────── */

#ifdef _WIN32
static DWORD WINAPI lsp_sender_thread_function(LPVOID args) {
#else
static void *lsp_sender_thread_function(void *args) {
#endif
    LSPContext    *ctx   = (LSPContext *)args;
    lsp_pipe_fd_t  fd    = ctx->write_fd;
    tiny_queue_t  *queue = ctx->sender_queue;

    while (ctx->running) {
        JsonValue *json_to_send = tiny_queue_pop(queue);
        if (!json_to_send) break;
        send_json_rpc_message(fd, json_to_send);
        json_free(json_to_send);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static DWORD WINAPI lsp_receiver_thread_function(LPVOID args) {
#else
static void *lsp_receiver_thread_function(void *args) {
#endif
    LSPContext    *ctx   = (LSPContext *)args;
    lsp_pipe_fd_t  fd    = ctx->read_fd;
    tiny_queue_t  *queue = ctx->receiver_queue;

    while (ctx->running) {
        JsonValue *response = receive_json_rpc_message(fd);
        if (!response) break;
        if (tiny_queue_push(queue, (void *)response) == -1) {
            perror("Could not push response to queue");
            json_free(response);
            break;
        }
    }
    tiny_queue_push(queue, NULL);  /* sentinel so lsp_wait_for can return */
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ── Thread create / join helpers ────────────────────────────────────────── */

static int lsp_thread_create(lsp_thread_t *t,
#ifdef _WIN32
                              LPTHREAD_START_ROUTINE fn,
#else
                              void *(*fn)(void *),
#endif
                              void *arg) {
#ifdef _WIN32
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return (*t == NULL) ? -1 : 0;
#else
    return pthread_create(t, NULL, fn, arg);
#endif
}

static void lsp_thread_join(lsp_thread_t t) {
#ifdef _WIN32
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
#else
    pthread_join(t, NULL);
#endif
}

/* ── Process spawning ────────────────────────────────────────────────────── */


/*
 * Spawns the LSP server process with its stdin/stdout connected to a pair of
 * anonymous pipes.  On success, *write_fd_out receives the write end (parent
 * writes → server reads) and *read_fd_out receives the read end (server
 * writes → parent reads).  Returns 0 on success, -1 on failure.
 */
static int lsp_start_server(lsp_pipe_fd_t *write_fd_out,
                             lsp_pipe_fd_t *read_fd_out,
                             LSPID lspid) {
    RegisteredLSP* lsp = lsp_id_table[lspid-1].value;

    const char *cmd = lsp->cmd;
    if (!cmd) {
        fprintf(stderr, "Error: Unknown LSP %s\n", lsp->name);
        return -1;
    }

#ifdef _WIN32
    /* ---- Windows -------------------------------------------------------- */
    HANDLE child_stdin_rd  = INVALID_HANDLE_VALUE;
    HANDLE child_stdin_wr  = INVALID_HANDLE_VALUE;
    HANDLE child_stdout_rd = INVALID_HANDLE_VALUE;
    HANDLE child_stdout_wr = INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE /* inheritable */ };

    if (!CreatePipe(&child_stdin_rd,  &child_stdin_wr,  &sa, 0) ||
        !CreatePipe(&child_stdout_rd, &child_stdout_wr, &sa, 0)) {
        perror("CreatePipe");
        return -1;
    }

    /* The parent's ends must NOT be inherited by the child */
    SetHandleInformation(child_stdin_wr,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_stdout_rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { 0 };
    si.cb          = sizeof(si);
    si.hStdInput   = child_stdin_rd;
    si.hStdOutput  = child_stdout_wr;
    si.hStdError   = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags     = STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = { 0 };

    /* CreateProcessA needs a mutable command line buffer */
    char cmdline[256];
    snprintf(cmdline, sizeof(cmdline), "%s", cmd);

    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL,
                             TRUE,               /* inherit handles */
                             0, NULL, NULL,
                             &si, &pi);

    /* The child now owns the other ends — close our copies */
    CloseHandle(child_stdin_rd);
    CloseHandle(child_stdout_wr);

    if (!ok) {
        fprintf(stderr, "CreateProcess failed for '%s' (error %lu)\n", cmd, GetLastError());
        CloseHandle(child_stdin_wr);
        CloseHandle(child_stdout_rd);
        return -1;
    }

    /* We don't need the process / thread handles */
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    *write_fd_out = child_stdin_wr;
    *read_fd_out  = child_stdout_rd;
    return 0;

#else
    /* ---- POSIX ---------------------------------------------------------- */
    int to_server[2];
    int from_server[2];

    if (pipe(to_server) == -1 || pipe(from_server) == -1) {
        perror("pipe");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }

    if (pid == 0) {
        /* Child: wire up stdin/stdout and exec the server */
        dup2(to_server[0],   STDIN_FILENO);
        dup2(from_server[1], STDOUT_FILENO);

        close(to_server[0]);  close(to_server[1]);
        close(from_server[0]); close(from_server[1]);

        execlp(cmd, cmd, NULL);
        perror("execlp failed");
        _exit(1);
    }

    /* Parent: close the child's ends */
    close(to_server[0]);
    close(from_server[1]);

    *write_fd_out = to_server[1];
    *read_fd_out  = from_server[0];
    return 0;
#endif
}

/* ── LSP lifecycle ───────────────────────────────────────────────────────── */


static void shutdown_lsp_server(bool *running,
                                tiny_queue_t *sender_queue,
                                tiny_queue_t *receiver_queue) {
    /* Send shutdown request */
    JsonValue *shutdown_req = make_base_message();
    json_add_child(shutdown_req, "id",     json_new_string("shutdown"));
    json_add_child(shutdown_req, "method", json_new_string("shutdown"));
    json_add_child(shutdown_req, "params", json_new_null());
    tiny_queue_push(sender_queue, (void *)shutdown_req);

    JsonValue *shutdown_resp = lsp_wait_for(receiver_queue, 0, "shutdown");
    if (shutdown_resp) {
        json_print(shutdown_resp, 4, 0);
        json_free(shutdown_resp);
    }

    /* Send exit notification */
    JsonValue *exit_notif = make_base_message();
    json_add_child(exit_notif, "method", json_new_string("exit"));
    json_add_child(exit_notif, "params", json_new_object());
    tiny_queue_push(sender_queue, (void *)exit_notif);

    /* Poison pill stops the sender thread */
    tiny_queue_push(sender_queue, NULL);
    *running = false;
}

/*
 * Start an LSP server of the given kind.
 *
 * Usage:
 *   LSPContext *ctx = start_lsp(LSPKIND_CLANGD, NULL);
 *   // push messages via ctx->sender_queue
 *   // pop responses via ctx->receiver_queue / lsp_wait_for()
 *   destroy_lsp(ctx);
 *   // On POSIX, call wait(NULL) afterwards to reap the child.
 */
LSPContext *start_lsp(LSPID lspid, JsonValue *initialize_params) {
    LSPContext *ctx = (LSPContext *)malloc(sizeof(LSPContext));
    if (!ctx) return NULL;

    ctx->running    = true;
    ctx->write_fd   = LSP_INVALID_FD;
    ctx->read_fd    = LSP_INVALID_FD;
    ctx->capabilities = NULL;

    if (lsp_start_server(&ctx->write_fd, &ctx->read_fd, lspid) != 0) {
        free(ctx);
        return NULL;
    }
    ctx->kind = lspid;

    ctx->sender_queue   = tiny_queue_create();
    ctx->receiver_queue = tiny_queue_create();

    if (lsp_thread_create(&ctx->sender_thread,   lsp_sender_thread_function,   ctx) != 0 ||
        lsp_thread_create(&ctx->receiver_thread, lsp_receiver_thread_function, ctx) != 0) {
        perror("Failed to create LSP threads");
        free(ctx);
        return NULL;
    }

    /* Handshake */
    tiny_queue_push(ctx->sender_queue,
                    (void *)make_initialize_message(0, "initialize", initialize_params));
    ctx->capabilities = lsp_wait_for(ctx->receiver_queue, 0, "initialize");
    tiny_queue_push(ctx->sender_queue, (void *)make_initialized_notification());

    return ctx;
}

void destroy_lsp(LSPContext *ctx) {
    if (!ctx) return;

    shutdown_lsp_server(&ctx->running, ctx->sender_queue, ctx->receiver_queue);

    lsp_thread_join(ctx->sender_thread);
    lsp_thread_join(ctx->receiver_thread);

    lsp_pipe_close(ctx->write_fd);
    lsp_pipe_close(ctx->read_fd);

    /* Drain and free anything left in the queues */
    while (1) {
        void *item = tiny_queue_pop_nowait(ctx->sender_queue);
        if (!item) break;
        json_free(item);
    }
    tiny_queue_destroy(ctx->sender_queue);

    while (1) {
        void *item = tiny_queue_pop_nowait(ctx->receiver_queue);
        if (!item) break;
        json_print(item, 4, 0);
        json_free(item);
    }
    tiny_queue_destroy(ctx->receiver_queue);

    json_free(ctx->capabilities);
    free(ctx);
}

#endif /* _H_LSP */
