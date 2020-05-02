#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "http.h"

/* constant-time string comparison */
#define cst_strcmp(m, c0, c1, c2, c3) \
    *(uint32_t *) m == ((c3 << 24) | (c2 << 16) | (c1 << 8) | c0)

#define CR '\r'
#define LF '\n'
#define CRLFCRLF "\r\n\r\n"

#define state_code(X)                                                     \
    X(_start), X(_method), X(_spaces_before_uri), X(_after_slash_in_uri), \
        X(_http), X(_http_H), X(_http_HT), X(_http_HTT), X(_http_HTTP),   \
        X(_first_major_digit), X(_major_digit), X(_first_minor_digit),    \
        X(_minor_digit), X(_spaces_after_digit), X(_almost_done)

#define define_enum(name, code) enum { code(enum_entry) } name
#define enum_entry(entry) s##entry

#define define_label_array(name, code) void *name[] = {code(label_entry)}
#define label_entry(entry) &&c##entry

#define interrupt_parse() \
    do {                  \
        r->pos = pi;      \
        r->state = state; \
        return EAGAIN;    \
    } while (0)

#define dispatch(i)                                  \
    do {                                             \
        if (i >= r->last)                            \
            interrupt_parse();                       \
        p = (uint8_t *) &r->buf[pi & (MAX_BUF - 1)]; \
        ch = *p;                                     \
        goto *conditions[state];                     \
    } while (0)

int http_parse_request_line(http_request_t *r)
{
    uint8_t ch, *p, *m;
    size_t pi;

    define_enum(state, state_code);
    /* cppcheck-suppress syntaxError */
    define_label_array(conditions, state_code);

    state = r->state;

    /* initialize pi */
    pi = r->pos;
    dispatch(pi);

/* HTTP methods: GET, HEAD, POST */
c_start:
    r->request_start = p;

    if (ch == CR || ch == LF)
        dispatch(++pi);

    if ((ch < 'A' || ch > 'Z') && ch != '_')
        return HTTP_PARSER_INVALID_METHOD;

    state = s_method;
    dispatch(++pi);

c_method:
    if (ch == ' ') {
        m = r->request_start;

        switch (p - m) {
        case 3:
            if (cst_strcmp(m, 'G', 'E', 'T', ' ')) {
                r->method = HTTP_GET;
                break;
            }
            break;

        case 4:
            if (cst_strcmp(m, 'P', 'O', 'S', 'T')) {
                r->method = HTTP_POST;
                break;
            }

            if (cst_strcmp(m, 'H', 'E', 'A', 'D')) {
                r->method = HTTP_HEAD;
                break;
            }
            break;

        default:
            r->method = HTTP_UNKNOWN;
            break;
        }
        state = s_spaces_before_uri;
        dispatch(++pi);
    }

    if ((ch < 'A' || ch > 'Z') && ch != '_')
        return HTTP_PARSER_INVALID_METHOD;
    dispatch(++pi);

/* space* before URI */
c_spaces_before_uri:
    if (ch == '/') {
        r->uri_start = p;
        state = s_after_slash_in_uri;
        dispatch(++pi);
    }

    switch (ch) {
    case ' ':
        break;
    default:
        return HTTP_PARSER_INVALID_REQUEST;
    }
    dispatch(++pi);

c_after_slash_in_uri:
    switch (ch) {
    case ' ':
        r->uri_end = p;
        state = s_http;
        break;
    default:
        break;
    }
    dispatch(++pi);

/* space+ after URI */
c_http:
    switch (ch) {
    case ' ':
        break;
    case 'H':
        state = s_http_H;
        break;
    default:
        return HTTP_PARSER_INVALID_REQUEST;
    }
    dispatch(++pi);

c_http_H:
    switch (ch) {
    case 'T':
        state = s_http_HT;
        break;
    default:
        return HTTP_PARSER_INVALID_REQUEST;
    }
    dispatch(++pi);

c_http_HT:
    switch (ch) {
    case 'T':
        state = s_http_HTT;
        break;
    default:
        return HTTP_PARSER_INVALID_REQUEST;
    }
    dispatch(++pi);

c_http_HTT:
    switch (ch) {
    case 'P':
        state = s_http_HTTP;
        break;
    default:
        return HTTP_PARSER_INVALID_REQUEST;
    }
    dispatch(++pi);

c_http_HTTP:
    switch (ch) {
    case '/':
        state = s_first_major_digit;
        break;
    default:
        return HTTP_PARSER_INVALID_REQUEST;
    }
    dispatch(++pi);

/* first digit of major HTTP version */
c_first_major_digit:
    if (ch < '1' || ch > '9')
        return HTTP_PARSER_INVALID_REQUEST;

    r->http_major = ch - '0';
    state = s_major_digit;
    dispatch(++pi);

/* major HTTP version or dot */
c_major_digit:
    if (ch == '.') {
        state = s_first_minor_digit;
        dispatch(++pi);
    }

    if (ch < '0' || ch > '9')
        return HTTP_PARSER_INVALID_REQUEST;

    r->http_major = r->http_major * 10 + ch - '0';
    dispatch(++pi);

/* first digit of minor HTTP version */
c_first_minor_digit:
    if (ch < '0' || ch > '9')
        return HTTP_PARSER_INVALID_REQUEST;

    r->http_minor = ch - '0';
    state = s_minor_digit;
    dispatch(++pi);

/* minor HTTP version or end of request line */
c_minor_digit:
    if (ch == CR) {
        state = s_almost_done;
        dispatch(++pi);
    }

    if (ch == LF)
        goto done;

    if (ch == ' ') {
        state = s_spaces_after_digit;
        dispatch(++pi);
    }

    if (ch < '0' || ch > '9')
        return HTTP_PARSER_INVALID_REQUEST;

    r->http_minor = r->http_minor * 10 + ch - '0';
    dispatch(++pi);

c_spaces_after_digit:
    switch (ch) {
    case ' ':
        break;
    case CR:
        state = s_almost_done;
        break;
    case LF:
        goto done;
    default:
        return HTTP_PARSER_INVALID_REQUEST;
    }
    dispatch(++pi);

/* end of request line */
c_almost_done:
    r->request_end = p - 1;
    switch (ch) {
    case LF:
        goto done;
    default:
        return HTTP_PARSER_INVALID_REQUEST;
    }

done:
    r->pos = pi + 1;

    if (!r->request_end)
        r->request_end = p;

    r->state = s_start;

    return 0;
}
#undef state_code
#undef label_entry

#define state_code(X)                                                    \
    X(_start), X(_key), X(_spaces_before_colon), X(_spaces_after_colon), \
        X(_value), X(_cr), X(_crlf), X(_crlfcr)
#define label_entry(entry) &&case##entry

int http_parse_request_body(http_request_t *r)
{
    uint8_t ch, *p;
    size_t pi;

    define_enum(state, state_code);
    define_label_array(conditions, state_code);

    state = r->state;
    assert(state == 0 && "state should be 0");

    http_header_t *hd;
    pi = r->pos;
    dispatch(pi);

case_start:
    if (ch == CR || ch == LF)
        dispatch(++pi);

    r->cur_header_key_start = p;
    state = s_key;
    dispatch(++pi);

case_key:
    if (ch == ' ') {
        r->cur_header_key_end = p;
        state = s_spaces_before_colon;
        dispatch(++pi);
    }

    if (ch == ':') {
        r->cur_header_key_end = p;
        state = s_spaces_after_colon;
        dispatch(++pi);
    }
    dispatch(++pi);

case_spaces_before_colon:
    if (ch == ' ')
        dispatch(++pi);
    if (ch == ':') {
        state = s_spaces_after_colon;
        dispatch(++pi);
    }
    return HTTP_PARSER_INVALID_HEADER;

case_spaces_after_colon:
    if (ch == ' ')
        dispatch(++pi);

    state = s_value;
    r->cur_header_value_start = p;
    dispatch(++pi);

case_value:
    if (ch == CR) {
        r->cur_header_value_end = p;
        state = s_cr;
    }

    if (ch == LF) {
        r->cur_header_value_end = p;
        state = s_crlf;
    }
    dispatch(++pi);

case_cr:
    if (ch == LF) {
        state = s_crlf;
        /* save the current HTTP header */
        hd = malloc(sizeof(http_header_t));
        hd->key_start = r->cur_header_key_start;
        hd->key_end = r->cur_header_key_end;
        hd->value_start = r->cur_header_value_start;
        hd->value_end = r->cur_header_value_end;

        list_add(&(hd->list), &(r->list));
        dispatch(++pi);
    }
    return HTTP_PARSER_INVALID_HEADER;

case_crlf:
    if (ch == CR) {
        state = s_crlfcr;
    } else {
        r->cur_header_key_start = p;
        state = s_key;
    }
    dispatch(++pi);

case_crlfcr:
    switch (ch) {
    case LF:
        goto done;
    default:
        return HTTP_PARSER_INVALID_HEADER;
    }
    dispatch(++pi);

done:
    r->pos = pi + 1;
    r->state = s_start;
    return 0;
}
