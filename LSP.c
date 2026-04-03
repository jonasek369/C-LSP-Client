#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <pthread.h>
#include "tiny_queue.h"

#define STB_DS_IMPLEMENTATION
#include "LSP.h"

static volatile bool running = true;


void send_json_rpc_message(int fd, JsonValue *packet) {
    char *out = NULL;

    json_dump(packet, &out);
    arrput(out, '\0');

    int len = strlen(out);
    char *buffer = malloc(len + 64);

    int n = snprintf(buffer, len + 64, "Content-Length: %d\r\n\r\n%s", len, out);
    write(fd, buffer, n);
    arrfree(out);
    free(buffer);
}

// Returns NULL on EOF or error
JsonValue *recieve_json_rpc_message(int fd) {
    char header[256] = {0};
    int pos = 0;

    while (1) {
        ssize_t n = read(fd, &header[pos], 1);
        if (n <= 0) return NULL;  // EOF or error — server has closed
        pos++;
        if (pos >= 4 && memcmp(header + pos - 4, "\r\n\r\n", 4) == 0)
            break;
        if (pos >= (int)sizeof(header) - 1) return NULL;  // header overflow
    }

    int content_length = 0;
    sscanf(header, "Content-Length: %d", &content_length);
    if (content_length <= 0) return NULL;

    char *body = malloc(content_length + 1);
    int total = 0;
    while (total < content_length) {
        ssize_t n = read(fd, body + total, content_length - total);
        if (n <= 0) { free(body); return NULL; }
        total += n;
    }
    body[content_length] = '\0';

    JsonValue *response = json_new_object();
    jsonStringLoad(body, response);
    free(body);
    return response;
}


static void* lsp_sender_thread_function(void *args) {
    int fd = ((LSPContext*)args)->write_read_fds[0];
    tiny_queue_t* queue = ((LSPContext*)args)->sender_queue;

    while (running) {
        JsonValue *json_to_send = tiny_queue_pop(queue);
        if (!json_to_send) break;
        send_json_rpc_message(fd, json_to_send);
        json_free(json_to_send);
    }
    return NULL;
}

static void* lsp_reciever_thread_function(void *args) {
    int fd = ((LSPContext*)args)->write_read_fds[1];
    tiny_queue_t* queue = ((LSPContext*)args)->reciever_queue;

    while (running) {
        JsonValue *response = recieve_json_rpc_message(fd);
        if (!response) break;  // EOF/error — server exited
        if (tiny_queue_push(queue, (void *)response) == -1) {
            perror("Could not push response to queue");
            json_free(response);
            break;
        }
    }
    tiny_queue_push(queue, NULL);
    return NULL;
}

void start_lsp_server(int *write_fd_out, int *read_fd_out) {
    int to_server[2];
    int from_server[2];

    if (pipe(to_server) == -1 || pipe(from_server) == -1) {
        perror("pipe");
        exit(1);
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        dup2(to_server[0], STDIN_FILENO);
        dup2(from_server[1], STDOUT_FILENO);

        close(to_server[0]);
        close(to_server[1]);
        close(from_server[0]);
        close(from_server[1]);

        execlp("clangd", "clangd", NULL);
        perror("execlp failed");
        exit(1);
    }

    close(to_server[0]);
    close(from_server[1]);

    if (write_fd_out) *write_fd_out = to_server[1];
    if (read_fd_out)  *read_fd_out  = from_server[0];
}

void shutdown_lsp_server(tiny_queue_t *sender_queue, tiny_queue_t *reciever_queue) {
    JsonValue *shutdown_req = json_new_object();
    json_add_child(shutdown_req, "jsonrpc", json_new_string("2.0"));
    json_add_child(shutdown_req, "id",      json_new_number(2));
    json_add_child(shutdown_req, "method",  json_new_string("shutdown"));
    json_add_child(shutdown_req, "params",  json_new_object());
    tiny_queue_push(sender_queue, (void *)shutdown_req);

    JsonValue *shutdown_resp = tiny_queue_pop(reciever_queue);
    if (shutdown_resp) {
        json_print(shutdown_resp, 4, 0);
        json_free(shutdown_resp);
    }

    JsonValue *exit_notif = json_new_object();
    json_add_child(exit_notif, "jsonrpc", json_new_string("2.0"));
    json_add_child(exit_notif, "method",  json_new_string("exit"));
    json_add_child(exit_notif, "params",  json_new_object());
    tiny_queue_push(sender_queue, (void *)exit_notif);

    tiny_queue_push(sender_queue, NULL);
    running = false;
}

LSPContext* start_lsp(){
    LSPContext* ctx = malloc(sizeof(LSPContext));
    ctx->write_read_fds = malloc(sizeof(int)*2);
    ctx->write_read_fds[0] = -1;
    ctx->write_read_fds[1] = -1;

    start_lsp_server(&(ctx->write_read_fds[0]), &(ctx->write_read_fds[1]));
    if(ctx->write_read_fds[0] == -1 || ctx->write_read_fds[1] == -1){
        free(ctx->write_read_fds);
        free(ctx);
        return NULL;
    }

    ctx->sender_queue = tiny_queue_create();
    ctx->reciever_queue = tiny_queue_create();

    if (pthread_create(&ctx->sender_thread, NULL, lsp_sender_thread_function, ctx) != 0) {
        perror("Failed to create sender thread");
        return NULL;
    }

    if (pthread_create(&ctx->reciever_thread, NULL, lsp_reciever_thread_function, ctx) != 0) {
        perror("Failed to create receiver thread");
        return NULL;
    }

    return ctx;
}

// must be only called only after shutdown lsp
void destroy_lsp(LSPContext* ctx){
    pthread_join(ctx->sender_thread, NULL);
    pthread_join(ctx->reciever_thread, NULL);

    close(ctx->write_read_fds[1]);
    close(ctx->write_read_fds[0]);

    while(1){
        void* item = tiny_queue_pop_nowait(ctx->sender_queue);
        if(!item){
            break;
        }
        free(item);
    }
    tiny_queue_destroy(ctx->sender_queue);
    while(1){
        void* item = tiny_queue_pop_nowait(ctx->reciever_queue);
        if(!item){
            break;
        }
        free(item);
    }
    tiny_queue_destroy(ctx->reciever_queue);

    free(ctx->write_read_fds);
    free(ctx);
}



JsonValue* make_base_message(){
    JsonValue* message = json_new_object();
    json_add_child(message, "jsonrpc", json_new_string("2.0"));
    return message;
}

JsonValue* make_initialize_message(double id, JsonValue* params){
    JsonValue* message = make_base_message();
    json_add_child(message, "id",      json_new_number(id));
    json_add_child(message, "method",  json_new_string("initialize"));
    json_add_child(message, "params", params == NULL ? json_new_object() : params);
    return message;
}