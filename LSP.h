#ifndef _H_LSP
#define _H_LSP

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <pthread.h>


/* ! Statics should be treated as private function and should not be used !

	Example:
	int main(void){
		// create context and start lsp
		LSPContext* ctx = start_lsp();

    	// Messages are send through pushing to sender queue

    	// messages start with int id and const char* str_id
    	// if str_id is NULL id is used
    	JsonValue* message = some_message_creating_function(0, "id_name");
    	tiny_queue_push(ctx->sender_queue, message);
		
		// responses are recieved by poping from receiver_queue
    	JsonValue *response = lsp_wait_for(ctx->receiver_queue, 0, "id_name");
    	if (response) {
    	    // handle response
    	    json_free(response);
    	}
	
    	sleep(1);
    	// Destroy ctx will shutdown lsp and free all memory
    	destroy_lsp(ctx);
    	wait(NULL); // to make sure lsp closes with main program
	}

*/
#ifndef LSP_NO_STB_DS
	#define STB_DS_IMPLEMENTATION
	#include "stb_ds.h"
#endif

#ifndef LSP_NO_CJSON
	#define CJSON_NO_STB_DS
	#include "parser.h"
#endif

#ifndef LSP_NO_TINY_QUEUE
	#include "tiny_queue.h"
#endif

typedef enum {
    LSPKIND_UNKNOWN = 0,
    LSPKIND_CLANGD,
    LSPKIND_PYLSP
} LSPKind;

typedef struct {
    volatile bool running;
    pthread_t sender_thread;
    tiny_queue_t* sender_queue;
    pthread_t reciever_thread;
    tiny_queue_t* receiver_queue;
    JsonValue* capabilities;

    int* write_read_fds;
    LSPKind kind;
} LSPContext;


/*

	Messages

*/

#ifndef LSP_WAIT_FOR_TIMEOUT_US
	#define LSP_WAIT_FOR_TIMEOUT_US 10
#endif

char* get_file_uri(const char* full_path){
    char* uri = NULL;
    arrput(uri, 'f');
    arrput(uri, 'i');
    arrput(uri, 'l');
    arrput(uri, 'e');
    arrput(uri, ':');
    arrput(uri, '/');
    arrput(uri, '/');
    for(size_t i = 0; i < strlen(full_path); i++){
        arrput(uri, full_path[i]);
    }
    arrput(uri, '\0');
    return uri;
}

JsonValue* lsp_wait_for(tiny_queue_t* queue, int id, const char* str_id){
    while(1){
        JsonValue* response = tiny_queue_pop(queue);
        if(!response){
            // sentinel
            return NULL;
        }
        JsonValue* response_id = shget(response->object, "id");
        if(
        	response_id 					 &&
        	response_id->type == JSON_NUMBER &&
        	str_id == NULL 					 &&
        	(int)(response_id->number) == id
        ){
            return response;
        }else if(
        	response_id                      &&
            response_id->type == JSON_STRING && 
            str_id != NULL 				     &&
            strncmp(response_id->string, str_id, strlen(response_id->string)) == 0
        )
            return response;
        else{
            tiny_queue_push(queue, response);
        }
        usleep(LSP_WAIT_FOR_TIMEOUT_US);
    }
}

JsonValue* make_base_message(){
    JsonValue* message = json_new_object();
    json_add_child(message, "jsonrpc", json_new_string("2.0"));
    return message;
}

#define MAKE_ID(id, str_id) str_id == NULL ? json_new_number(id) : json_new_string(str_id)

JsonValue* make_initialize_message(double id, const char* str_id, JsonValue* params){
    JsonValue* message = make_base_message();
    JsonValue* message_id = MAKE_ID(id, str_id);
    json_add_child(message, "id",     message_id);
    json_add_child(message, "method", json_new_string("initialize"));
    json_add_child(message, "params", params == NULL ? json_new_object() : params);
    return message;
}

JsonValue* make_didOpen_params(const char* document_uri, const char* language_id, int version, const char* text){
	JsonValue* params = json_new_object();
	JsonValue* text_document = json_new_object();
	json_add_child(text_document, "uri", json_new_string(document_uri));
	json_add_child(text_document, "languageId", json_new_string(language_id));
	json_add_child(text_document, "version", json_new_number(version));
	json_add_child(text_document, "text", json_new_string(text));
	json_add_child(params, "textDocument", text_document);
	return params;
}

JsonValue* make_didOpen_notification(JsonValue* params){
	if(!params){
		return NULL;
	}
	JsonValue* message = make_base_message();
    json_add_child(message, "method", json_new_string("textDocument/didOpen"));
    json_add_child(message, "params", params);
    return message;
}

JsonValue* make_didChange_params(const char* document_uri, int version, const char* text){
    JsonValue* params = json_new_object();
    JsonValue* text_document = json_new_object();
    JsonValue* content_changes = json_new_array();
    JsonValue* change = json_new_object();
    json_add_child(text_document, "uri", json_new_string(document_uri));
    json_add_child(text_document, "version", json_new_number(version));

    json_add_child(params, "textDocument", text_document);

    json_add_child(change, "text", json_new_string(text));
    json_add_child(content_changes, NULL, change);

    json_add_child(params, "contentChanges", content_changes);
    return params;
}

JsonValue* make_didChange_notification(JsonValue* params){
    if(!params){
        return NULL;
    }
    JsonValue* message = make_base_message();
    json_add_child(message, "method", json_new_string("textDocument/didChange"));
    json_add_child(message, "params", params);
    return message;
}

JsonValue* make_completion_params(const char* document_uri, unsigned int line, unsigned int character) {
    JsonValue* params = json_new_object();

    JsonValue* textDocument = json_new_object();
    json_add_child(textDocument, "uri", json_new_string(document_uri));
    json_add_child(params, "textDocument", textDocument);

    JsonValue* position = json_new_object();
    json_add_child(position, "line", json_new_number(line));
    json_add_child(position, "character", json_new_number(character));
    json_add_child(params, "position", position);

    json_add_child(params, "workDoneToken", json_new_null());

    JsonValue* context = json_new_object();
    json_add_child(context, "triggerKind", json_new_number(1));  // invoked
    json_add_child(context, "triggerCharacter", json_new_null());
    json_add_child(params, "context", context);

    return params;
}

JsonValue* make_completion_request(JsonValue* id, JsonValue* params){
    JsonValue* message = make_base_message();
    json_add_child(message, "id", id);
    json_add_child(message, "method", json_new_string("textDocument/completion"));
    json_add_child(message, "params", params);
    return message;   
}

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
static JsonValue* recieve_json_rpc_message(int fd) {
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

    while (((LSPContext*)args)->running) {
        JsonValue *json_to_send = tiny_queue_pop(queue);
        if (!json_to_send) break;
        send_json_rpc_message(fd, json_to_send);
        json_free(json_to_send);
    }
    return NULL;
}

static void* lsp_reciever_thread_function(void *args) {
    int fd = ((LSPContext*)args)->write_read_fds[1];
    tiny_queue_t* queue = ((LSPContext*)args)->receiver_queue;

    while (((LSPContext*)args)->running) {
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

static void start_lsp_kind(LSPKind kind) {
    switch (kind) {
        case LSPKIND_CLANGD:
            execlp("clangd", "clangd", NULL);
            break;

        case LSPKIND_PYLSP:
            execlp("pylsp", "pylsp", NULL);
            break;

        default:
            fprintf(stderr, "Error: Unknown %d kind of LSP. Could not start\n", kind);
            exit(1);
    }
}

static void start_lsp_server(int *write_fd_out, int *read_fd_out, LSPKind kind) {
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

        start_lsp_kind(kind);
        perror("execlp failed");
        exit(1);
    }

    close(to_server[0]);
    close(from_server[1]);

    if (write_fd_out) *write_fd_out = to_server[1];
    if (read_fd_out)  *read_fd_out  = from_server[0];
}

static void shutdown_lsp_server(bool* running, tiny_queue_t *sender_queue, tiny_queue_t *receiver_queue) {
    JsonValue *shutdown_req = make_base_message();
    json_add_child(shutdown_req, "id",      json_new_string("shutdown"));
    json_add_child(shutdown_req, "method",  json_new_string("shutdown"));
    json_add_child(shutdown_req, "params",  json_new_object());
    tiny_queue_push(sender_queue, (void *)shutdown_req);

    JsonValue *shutdown_resp = lsp_wait_for(receiver_queue, 0, "shutdown");
    if (shutdown_resp) {
        json_print(shutdown_resp, 4, 0);
        json_free(shutdown_resp);
    }

    JsonValue *exit_notif = make_base_message();
    json_add_child(exit_notif, "method",  json_new_string("exit"));
    json_add_child(exit_notif, "params",  json_new_object());
    tiny_queue_push(sender_queue, (void *)exit_notif);

    tiny_queue_push(sender_queue, NULL);
    *running = false;
}

LSPContext* start_lsp(LSPKind kind){
    LSPContext* ctx = malloc(sizeof(LSPContext));
    ctx->running = true;
    ctx->write_read_fds = malloc(sizeof(int)*2);
    ctx->write_read_fds[0] = -1;
    ctx->write_read_fds[1] = -1;

    // TODO: Add more lsps
    start_lsp_server(&(ctx->write_read_fds[0]), &(ctx->write_read_fds[1]), kind);
    ctx->kind = kind;
    if(ctx->write_read_fds[0] == -1 || ctx->write_read_fds[1] == -1){
        free(ctx->write_read_fds);
        free(ctx);
        return NULL;
    }

    ctx->sender_queue = tiny_queue_create();
    ctx->receiver_queue = tiny_queue_create();
    ctx->capabilities = NULL;

    if (pthread_create(&ctx->sender_thread, NULL, lsp_sender_thread_function, ctx) != 0) {
        perror("Failed to create sender thread");
        return NULL;
    }

    if (pthread_create(&ctx->reciever_thread, NULL, lsp_reciever_thread_function, ctx) != 0) {
        perror("Failed to create receiver thread");
        return NULL;
    }

    tiny_queue_push(ctx->sender_queue, (void*)make_initialize_message(0, "initialize", NULL));
    ctx->capabilities = lsp_wait_for(ctx->receiver_queue, 0, "initialize");

    return ctx;
}

void destroy_lsp(LSPContext* ctx){
	shutdown_lsp_server(&ctx->running, ctx->sender_queue, ctx->receiver_queue);

    pthread_join(ctx->sender_thread, NULL);
    pthread_join(ctx->reciever_thread, NULL);

    close(ctx->write_read_fds[1]);
    close(ctx->write_read_fds[0]);

    while(1){
        void* item = tiny_queue_pop_nowait(ctx->sender_queue);
        if(!item){
            break;
        }
        json_free(item);
    }
    tiny_queue_destroy(ctx->sender_queue);
    while(1){
        void* item = tiny_queue_pop_nowait(ctx->receiver_queue);
        if(!item){
            break;
        }
        json_print(item, 4, 0);
        json_free(item);
    }
    tiny_queue_destroy(ctx->receiver_queue);

    free(ctx->write_read_fds);
    json_free(ctx->capabilities);
    free(ctx);
}

#endif //_H_LSP
