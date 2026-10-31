#define STB_DS_IMPLEMENTATION
#include "../thirdparty/stb_ds.h"
#define CJSON_NO_STB_DS
#include "../C-JSON/parser.h"
#include "../tiny_queue/tiny_queue.h"

#define LSP_NO_STB_DS
#define LSP_NO_CJSON
#define LSP_NO_TINY_QUEUE
#include "LSP.h"
    
#define NOB_IMPLEMENTATION
#include "../thirdparty/nob.h"


int main(void){
    LSPID clangd = register_lsp("clangd", "clangd");

    LSPContext *ctx = start_lsp(clangd, NULL);
    Nob_String_Builder sb = {0};

    nob_read_entire_file("test.c", &sb);
    nob_sb_append_null(&sb);
    char* escaped = json_escape(sb.items);
    Nob_String_Builder path_sb = {0};
    const char* cwd = nob_get_current_dir_temp();
    nob_sb_append_cstr(&path_sb, "file://");
    nob_sb_append_cstr(&path_sb, cwd);
    nob_sb_append_cstr(&path_sb, "/test.c");

    JsonValue* params = make_didOpen_params(path_sb.items, "c", 1, escaped);
    JsonValue* message = make_didOpen_notification(params);
    printf("[C-LSP-Client] Sending didOpen notification to LSP:\n");
    json_print(message, 4, 0);
    tiny_queue_push(ctx->sender_queue, message);
    free(escaped);
    NOB_FREE(sb.items);
    NOB_FREE(path_sb.items);
    printf("[C-LSP-Client] Waiting for response from LSP:\n");
    void* item = tiny_queue_pop(ctx->receiver_queue);
    if(item){
        printf("[C-LSP-Client] Response from LSP:\n");
        json_print((JsonValue*)item, 4, 0);
    }
    destroy_lsp(ctx);
    destroy_registered_lsps();
    wait(NULL);
}