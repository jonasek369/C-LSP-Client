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

char *json_escape(const char *input) {
    if (!input) return NULL;

    size_t len = strlen(input);
    char *escaped = malloc(len * 6 + 1);
    if (!escaped) return NULL;

    char *p = escaped;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = input[i];
        switch (c) {
            case '\"': *p++ = '\\'; *p++ = '\"'; break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\b': *p++ = '\\'; *p++ = 'b';  break;
            case '\f': *p++ = '\\'; *p++ = 'f';  break;
            case '\n': *p++ = '\\'; *p++ = 'n';  break;
            case '\r': *p++ = '\\'; *p++ = 'r';  break;
            case '\t': *p++ = '\\'; *p++ = 't';  break;
            default:
                if (c < 0x20) {
                    sprintf(p, "\\u%04x", c);
                    p += 6;
                } else {
                    *p++ = c;
                }
                break;
        }
    }
    *p = '\0';
    return escaped;
}

int main(void){
    LSPContext* ctx = start_lsp();
    Nob_String_Builder sb = {0};

    nob_read_entire_file("test.c", &sb);
    nob_sb_append_null(&sb);
    char* escaped = json_escape(sb.items);

    JsonValue* params = make_didOpen_params("file:///home/jonas/programming/LSP-test/test.c", "c", 1, escaped);
    JsonValue* message = make_didOpen_notification(params);
    json_print(message, 4, 0);
    tiny_queue_push(ctx->sender_queue, message);
    free(escaped);
    NOB_FREE(sb.items);

    sleep(2);
    destroy_lsp(ctx);
    wait(NULL);
}