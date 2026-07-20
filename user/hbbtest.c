/* =============================================================================
 * hbbtest.c — dyn musl program exercising libhubbub (§M42 NetSurf libs).
 * Tokenises a small HTML document and counts start tags via the token handler
 * callback — real HTML parsing.  Proves the ported libhubbub.so.0 (a store
 * package, DT_NEEDED libhubbub.so.0 -> libparserutils.so.0) loads + runs.
 * ============================================================================= */
#include <stdio.h>
#include <string.h>
#include <hubbub/parser.h>

static int start_tags = 0;

static hubbub_error on_token(const hubbub_token *token, void *pw) {
    (void)pw;
    if (token->type == HUBBUB_TOKEN_START_TAG) start_tags++;
    return HUBBUB_OK;
}

int main(void) {
    hubbub_parser *parser = NULL;
    if (hubbub_parser_create("UTF-8", false, &parser) != HUBBUB_OK || !parser) {
        printf("hbbtest: hubbub_parser_create failed\n");
        return 1;
    }
    hubbub_parser_optparams params;
    params.token_handler.handler = on_token;
    params.token_handler.pw = NULL;
    hubbub_parser_setopt(parser, HUBBUB_PARSER_TOKEN_HANDLER, &params);

    const char *html = "<html><head></head><body><p>hi</p></body></html>";
    hubbub_parser_parse_chunk(parser, (const uint8_t *)html, strlen(html));
    hubbub_parser_destroy(parser);

    printf("hbbtest: libhubbub parsed HTML, %d start tags (html/head/body/p)\n", start_tags);
    return (start_tags == 4) ? 0 : 1;
}
