/*
 * quickts: JSX lowering ("quicktsx").
 *
 *   <tag a={x} b="y" flag>{child}text</tag>
 *     ->  FACTORY("tag", {"a": x, "b": "y", "flag": true}, child, "text")
 *   <>a</>
 *     ->  FACTORY(FRAGMENT, null, "a")
 *
 * The factory receives (tag, props-or-null, ...children); the names default
 * to FrameOS's __frameosJsx / __frameosFragment and are build-time defines.
 *
 * With JS_EVAL_FLAG_JSX_BLOCKS a site lowers to one call of BLOCK with a
 * static template and its expressions instead ("Block lowering", below).
 *
 * JSX children are not JavaScript tokens, so this scans raw source bytes and
 * hands control back to the real tokenizer for every embedded expression:
 * point s->buf_ptr at the byte after '{', call next_token(), let
 * js_parse_assign_expr() emit the value, then resume raw scanning from
 * s->buf_ptr (which sits one past the closing '}'). That is the whole trick
 * that avoids a second tokenizer mode.
 *
 * Included from quickts.h.
 */

#ifndef TS_JSX_FACTORY
#define TS_JSX_FACTORY  "__frameosJsx"
#endif
#ifndef TS_JSX_FRAGMENT
#define TS_JSX_FRAGMENT "__frameosFragment"
#endif
#ifndef TS_JSX_BLOCK
#define TS_JSX_BLOCK    "__frameosBlock"
#endif

#define TS_JSX_MAX_DEPTH 32

static __exception int js_parse_assign_expr(JSParseState *s);

typedef struct {
    char *buf;
    size_t len;
    size_t size;
    BOOL failed;
} TSBuf;

static void ts_buf_init(TSBuf *b)
{
    b->buf = NULL;
    b->len = 0;
    b->size = 0;
    b->failed = FALSE;
}

static void ts_buf_free(JSContext *ctx, TSBuf *b)
{
    js_free(ctx, b->buf);
    b->buf = NULL;
}

static void ts_buf_putc(JSContext *ctx, TSBuf *b, int c)
{
    if (b->failed)
        return;
    if (b->len + 1 > b->size) {
        size_t size = b->size ? b->size * 2 : 64;
        char *buf = js_realloc(ctx, b->buf, size);
        if (!buf) {
            b->failed = TRUE;
            return;
        }
        b->buf = buf;
        b->size = size;
    }
    b->buf[b->len++] = (char)c;
}

static void ts_buf_put(JSContext *ctx, TSBuf *b, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        ts_buf_putc(ctx, b, s[i]);
}

static void ts_buf_put_utf8(JSContext *ctx, TSBuf *b, uint32_t c)
{
    if (c < 0x80) {
        ts_buf_putc(ctx, b, c);
    } else if (c < 0x800) {
        ts_buf_putc(ctx, b, 0xc0 | (c >> 6));
        ts_buf_putc(ctx, b, 0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        ts_buf_putc(ctx, b, 0xe0 | (c >> 12));
        ts_buf_putc(ctx, b, 0x80 | ((c >> 6) & 0x3f));
        ts_buf_putc(ctx, b, 0x80 | (c & 0x3f));
    } else {
        ts_buf_putc(ctx, b, 0xf0 | (c >> 18));
        ts_buf_putc(ctx, b, 0x80 | ((c >> 12) & 0x3f));
        ts_buf_putc(ctx, b, 0x80 | ((c >> 6) & 0x3f));
        ts_buf_putc(ctx, b, 0x80 | (c & 0x3f));
    }
}

/* JSX scanning runs ahead of the tokenizer, so s->token is stale; report
   errors against the raw pointer instead. */
#define ts_jsx_error(s, p, msg) js_parse_error_pos(s, p, "%s", msg)

static BOOL ts_jsx_space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* Inside a tag, `//` and block comments are comments, not text. */
static const uint8_t *ts_jsx_skip_space(const uint8_t *p, const uint8_t *end)
{
    for (;;) {
        while (p < end && ts_jsx_space(*p))
            p++;
        if (p + 1 < end && p[0] == '/' && p[1] == '/') {
            p += 2;
            while (p < end && *p != '\n')
                p++;
            continue;
        }
        if (p + 1 < end && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
                p++;
            p = (p + 1 < end) ? p + 2 : end;
            continue;
        }
        return p;
    }
}

/* `<Comp<T> prop={x} />`: TypeScript type arguments on a JSX element. The
   region is balanced angle brackets; strings inside it are skipped so a
   literal type like `Foo<">">` cannot end it early. */
static const uint8_t *ts_jsx_skip_type_args(const uint8_t *p, const uint8_t *end)
{
    int depth = 0;

    if (p >= end || *p != '<')
        return p;
    for (;;) {
        if (p >= end)
            return NULL;
        if (*p == '"' || *p == '\'' || *p == '`') {
            int quote = *p++;
            while (p < end && *p != quote) {
                if (*p == '\\' && p + 1 < end)
                    p++;
                p++;
            }
            if (p >= end)
                return NULL;
            p++;
            continue;
        }
        if (*p == '<') {
            depth++;
        } else if (*p == '>') {
            depth--;
            if (depth == 0)
                return p + 1;
        } else if (*p == '{' || *p == '}' || *p == ';') {
            return NULL;        /* not type arguments after all */
        }
        p++;
    }
}

static BOOL ts_jsx_name_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '$' ||
        c == '-' || c == ':' || c == '.';
}

/* &amp; &lt; &gt; &quot; &apos; &nbsp; &#NN; &#xHH; -- anything else is
   copied through verbatim. */
static void ts_jsx_entities(JSContext *ctx, TSBuf *out, const char *in, size_t len)
{
    size_t i = 0;

    while (i < len) {
        size_t semi;
        size_t elen;
        const char *e;

        if (in[i] != '&') {
            ts_buf_putc(ctx, out, in[i]);
            i++;
            continue;
        }
        for (semi = i + 1; semi < len && in[semi] != ';'; semi++)
            continue;
        if (semi >= len) {
            ts_buf_putc(ctx, out, in[i]);
            i++;
            continue;
        }
        e = in + i + 1;
        elen = semi - i - 1;
        if (elen == 3 && !memcmp(e, "amp", 3)) {
            ts_buf_putc(ctx, out, '&');
        } else if (elen == 2 && !memcmp(e, "lt", 2)) {
            ts_buf_putc(ctx, out, '<');
        } else if (elen == 2 && !memcmp(e, "gt", 2)) {
            ts_buf_putc(ctx, out, '>');
        } else if (elen == 4 && !memcmp(e, "quot", 4)) {
            ts_buf_putc(ctx, out, '"');
        } else if (elen == 4 && !memcmp(e, "apos", 4)) {
            ts_buf_putc(ctx, out, '\'');
        } else if (elen == 4 && !memcmp(e, "nbsp", 4)) {
            ts_buf_putc(ctx, out, ' ');
        } else if (elen >= 2 && e[0] == '#') {
            uint32_t v = 0;
            size_t k = 1;
            int base = 10;
            BOOL ok = TRUE;
            if (e[1] == 'x' || e[1] == 'X') {
                base = 16;
                k = 2;
            }
            if (k >= elen)
                ok = FALSE;
            for (; k < elen && ok; k++) {
                int d = e[k];
                if (d >= '0' && d <= '9')
                    d -= '0';
                else if (base == 16 && d >= 'a' && d <= 'f')
                    d = d - 'a' + 10;
                else if (base == 16 && d >= 'A' && d <= 'F')
                    d = d - 'A' + 10;
                else
                    ok = FALSE;
                if (ok)
                    v = v * base + d;
                if (v > 0x10ffff)
                    ok = FALSE;
            }
            if (ok)
                ts_buf_put_utf8(ctx, out, v);
            else
                ts_buf_put(ctx, out, in + i, semi - i + 1);
        } else {
            ts_buf_put(ctx, out, in + i, semi - i + 1);
        }
        i = semi + 1;
    }
}

/* JSX text, as TypeScript and Babel read it: whitespace that touches a line
   break goes, a line with nothing else on it goes, and the lines left are
   joined with a single space before entities are decoded. Whitespace with no
   line break in it stays: `{n} items` keeps " items", `<b>a</b> <b>c</b>`
   keeps the " " between them and `<a> x </a>` is " x ". */
static void ts_jsx_text(JSContext *ctx, TSBuf *out, const char *in, size_t len)
{
    TSBuf raw;
    size_t i;
    /* first non-space of the line, or none yet on a line after a break; the
       first line starts at 0 so its leading whitespace is kept */
    BOOL have_first = TRUE;
    size_t first = 0;
    BOOL have_last = FALSE; /* a non-space seen anywhere so far */
    size_t last = 0;
    BOOL joined = FALSE;

    ts_buf_init(&raw);
    for (i = 0; i < len; i++) {
        int c = (uint8_t)in[i];
        if (c == '\n' || c == '\r') {
            if (have_first && have_last) {
                if (joined)
                    ts_buf_putc(ctx, &raw, ' ');
                ts_buf_put(ctx, &raw, in + first, last - first + 1);
                joined = TRUE;
            }
            have_first = FALSE;
        } else if (!ts_jsx_space(c)) {
            last = i;
            have_last = TRUE;
            if (!have_first) {
                first = i;
                have_first = TRUE;
            }
        }
    }
    /* the last line keeps its trailing whitespace; a last line after a break
       with nothing on it never set `first`. A run with no break at all is
       kept whole, even when it is only spaces. */
    if (have_first) {
        if (joined)
            ts_buf_putc(ctx, &raw, ' ');
        ts_buf_put(ctx, &raw, in + first, len - first);
    }
    if (!raw.failed && raw.len > 0)
        ts_jsx_entities(ctx, out, raw.buf, raw.len);
    if (raw.failed)
        out->failed = TRUE;
    ts_buf_free(ctx, &raw);
}

static __exception int ts_jsx_emit_buf(JSParseState *s, TSBuf *b)
{
    if (b->failed)
        return -1;
    return ts_emit_string(s, b->buf ? b->buf : "", b->len);
}

/* Hand the byte at *pp (which must be '{') to the real parser, let it emit
   one assignment expression, and resume raw scanning after the '}'.
   *pempty is set when the braces held nothing but whitespace/comments. */
static __exception int ts_jsx_embedded_expr(JSParseState *s, const uint8_t **pp,
                                            BOOL *pempty, BOOL allow_spread)
{
    s->buf_ptr = *pp + 1;
    if (next_token(s))
        return -1;
    if (s->token.val == '}') {
        *pempty = TRUE;
        *pp = s->buf_ptr;
        return 0;
    }
    if (allow_spread && s->token.val == TOK_ELLIPSIS) {
        if (next_token(s))
            return -1;
    }
    if (js_parse_assign_expr(s))
        return -1;
    if (s->token.val != '}')
        return js_parse_error(s, "expecting '}' in JSX expression");
    *pempty = FALSE;
    *pp = s->buf_ptr;
    return 0;
}

static __exception int ts_jsx_element(JSParseState *s, const uint8_t **pp,
                                      int depth);

/* Children, up to the matching closing tag. Each child leaves one value on
   the stack; returns how many, or -1. */
static int ts_jsx_children(JSParseState *s, const uint8_t **pp,
                           const char *name, size_t name_len, int depth)
{
    const uint8_t *p = *pp;
    const uint8_t *end = s->buf_end;
    int count = 0;

    for (;;) {
        if (p >= end)
            return ts_jsx_error(s, p, "unterminated JSX element");
        if (p[0] == '<' && p + 1 < end && p[1] == '/') {
            const uint8_t *q = p + 2;
            const uint8_t *nstart = q;
            while (q < end && ts_jsx_name_char(*q))
                q++;
            if (name_len != (size_t)(q - nstart) ||
                (name_len && memcmp(nstart, name, name_len) != 0))
                return ts_jsx_error(s, p, "mismatched JSX closing tag");
            q = ts_jsx_skip_space(q, end);
            if (q >= end || *q != '>')
                return ts_jsx_error(s, q, "expecting '>' in JSX closing tag");
            *pp = q + 1;
            return count;
        }
        if (p[0] == '<') {
            if (ts_jsx_element(s, &p, depth + 1))
                return -1;
            count++;
            continue;
        }
        if (p[0] == '{') {
            BOOL empty;
            if (ts_jsx_embedded_expr(s, &p, &empty, FALSE))
                return -1;
            if (!empty)
                count++;
            continue;
        }
        {
            const uint8_t *start = p;
            TSBuf text;
            int ret;
            while (p < end && *p != '<' && *p != '{')
                p++;
            ts_buf_init(&text);
            ts_jsx_text(s->ctx, &text, (const char *)start, p - start);
            if (text.failed) {
                ts_buf_free(s->ctx, &text);
                return -1;
            }
            if (text.len == 0) {
                ts_buf_free(s->ctx, &text);
                continue;
            }
            ret = ts_jsx_emit_buf(s, &text);
            ts_buf_free(s->ctx, &text);
            if (ret)
                return -1;
            count++;
        }
    }
}

/* Attributes, leaving one value on the stack: the props object, or null. */
static __exception int ts_jsx_props(JSParseState *s, const uint8_t **pp, int depth)
{
    const uint8_t *p = *pp;
    const uint8_t *end = s->buf_end;
    BOOL any;

    p = ts_jsx_skip_space(p, end);
    any = (p < end && *p != '>' && *p != '/');
    if (!any) {
        emit_op(s, OP_null);
        *pp = p;
        return 0;
    }
    emit_op(s, OP_object);

    for (;;) {
        p = ts_jsx_skip_space(p, end);
        if (p >= end)
            return ts_jsx_error(s, p, "unterminated JSX attributes");
        if (*p == '>' || *p == '/')
            break;
        if (*p == '{') {
            /* {...spread} */
            BOOL empty;
            if (ts_jsx_embedded_expr(s, &p, &empty, TRUE))
                return -1;
            if (!empty) {
                emit_op(s, OP_null);        /* dummy excludeList */
                emit_op(s, OP_copy_data_properties);
                emit_u8(s, 2 | (1 << 2) | (0 << 5));
                emit_op(s, OP_drop);        /* excludeList */
                emit_op(s, OP_drop);        /* source */
            }
            continue;
        }
        {
            const uint8_t *nstart = p;
            JSAtom name;
            int ret;
            while (p < end && ts_jsx_name_char(*p))
                p++;
            if (p == nstart)
                return ts_jsx_error(s, p, "invalid JSX attribute name");
            name = JS_NewAtomLen(s->ctx, (const char *)nstart, p - nstart);
            if (name == JS_ATOM_NULL)
                return -1;
            p = ts_jsx_skip_space(p, end);
            if (p >= end || *p != '=') {
                emit_op(s, OP_push_true);        /* bare attribute */
            } else {
                p++;
                p = ts_jsx_skip_space(p, end);
                if (p < end && (*p == '"' || *p == '\'')) {
                    int quote = *p++;
                    const uint8_t *vstart = p;
                    TSBuf val;
                    while (p < end && *p != quote)
                        p++;
                    if (p >= end) {
                        JS_FreeAtom(s->ctx, name);
                        return ts_jsx_error(s, p, "unterminated JSX attribute");
                    }
                    ts_buf_init(&val);
                    ts_jsx_entities(s->ctx, &val, (const char *)vstart, p - vstart);
                    p++;                    /* closing quote */
                    ret = ts_jsx_emit_buf(s, &val);
                    ts_buf_free(s->ctx, &val);
                    if (ret) {
                        JS_FreeAtom(s->ctx, name);
                        return -1;
                    }
                } else if (p < end && *p == '{') {
                    BOOL empty;
                    if (ts_jsx_embedded_expr(s, &p, &empty, FALSE)) {
                        JS_FreeAtom(s->ctx, name);
                        return -1;
                    }
                    if (empty)
                        emit_op(s, OP_push_true);
                } else if (p < end && *p == '<') {
                    if (ts_jsx_element(s, &p, depth + 1)) {
                        JS_FreeAtom(s->ctx, name);
                        return -1;
                    }
                } else {
                    JS_FreeAtom(s->ctx, name);
                    return ts_jsx_error(s, p, "invalid JSX attribute value");
                }
            }
            emit_op(s, OP_define_field);
            emit_atom(s, name);
            JS_FreeAtom(s->ctx, name);
        }
    }
    *pp = p;
    return 0;
}

/* A tag name: lower-case first letter, or one containing '-' or ':', is a
   string; anything else is an expression (`Foo`, `Foo.Bar`). */
static BOOL ts_jsx_tag_is_string(const char *name, size_t len)
{
    size_t i;

    if (len == 0 || (name[0] >= 'a' && name[0] <= 'z'))
        return TRUE;
    for (i = 0; i < len; i++)
        if (name[i] == '-' || name[i] == ':')
            return TRUE;
    return FALSE;
}

static __exception int ts_jsx_emit_tag(JSParseState *s, const char *name, size_t len)
{
    if (ts_jsx_tag_is_string(name, len))
        return ts_emit_string(s, name, len);

    /* `Foo` or `Foo.Bar.Baz` */
    {
        size_t start = 0, k = 0;
        JSAtom atom;
        while (k <= len) {
            if (k == len || name[k] == '.') {
                atom = JS_NewAtomLen(s->ctx, name + start, k - start);
                if (atom == JS_ATOM_NULL)
                    return -1;
                if (start == 0) {
                    ts_emit_get_var(s, atom);
                } else {
                    emit_op(s, OP_get_field);
                    emit_atom(s, atom);
                }
                JS_FreeAtom(s->ctx, atom);
                start = k + 1;
            }
            k++;
        }
    }
    return 0;
}

/* *pp points at '<'. Emits FACTORY(tag, props, ...children) and leaves *pp
   just past the element. */
static __exception int ts_jsx_element(JSParseState *s, const uint8_t **pp,
                                      int depth)
{
    const uint8_t *p = *pp;
    const uint8_t *end = s->buf_end;
    JSAtom factory;
    const uint8_t *nstart;
    size_t name_len;
    int children, argc = 2;

    if (depth > TS_JSX_MAX_DEPTH)
        return ts_jsx_error(s, p, "JSX nested too deeply");
    if (p >= end || *p != '<')
        return ts_jsx_error(s, p, "expecting JSX element");
    p++;

    factory = JS_NewAtom(s->ctx, TS_JSX_FACTORY);
    if (factory == JS_ATOM_NULL)
        return -1;
    ts_emit_get_var(s, factory);
    JS_FreeAtom(s->ctx, factory);

    if (p < end && *p == '>') {
        /* <>...</> */
        JSAtom fragment = JS_NewAtom(s->ctx, TS_JSX_FRAGMENT);
        if (fragment == JS_ATOM_NULL)
            return -1;
        ts_emit_get_var(s, fragment);
        JS_FreeAtom(s->ctx, fragment);
        emit_op(s, OP_null);
        p++;
        children = ts_jsx_children(s, &p, NULL, 0, depth);
        if (children < 0)
            return -1;
        argc += children;
        goto emit_call;
    }

    nstart = p;
    while (p < end && ts_jsx_name_char(*p))
        p++;
    name_len = p - nstart;
    if (name_len == 0)
        return ts_jsx_error(s, p, "invalid JSX tag name");
    if (ts_jsx_emit_tag(s, (const char *)nstart, name_len))
        return -1;

    if (s->ts && p < end && *p == '<') {
        const uint8_t *after = ts_jsx_skip_type_args(p, end);
        if (after)
            p = after;
    }

    if (ts_jsx_props(s, &p, depth))
        return -1;

    if (p + 1 < end && p[0] == '/' && p[1] == '>') {
        p += 2;
        goto emit_call;
    }
    if (p >= end || *p != '>')
        return ts_jsx_error(s, p, "expecting '>' in JSX element");
    p++;
    children = ts_jsx_children(s, &p, (const char *)nstart, name_len, depth);
    if (children < 0)
        return -1;
    argc += children;

 emit_call:
    if (argc > 65535)
        return ts_jsx_error(s, p, "too many JSX children");
    emit_op(s, OP_call);
    emit_u16(s, argc);
    *pp = p;
    return 0;
}


/* ---- Block lowering (JS_EVAL_FLAG_JSX_BLOCKS) ------------------------------
 *
 * A JSX site becomes one call: what never changes is gathered into a constant
 * (the template) and the expressions are passed as arguments (the holes):
 *
 *   <tag a={x} b="y" flag>{child}text<i /></tag>
 *     ->  BLOCK(T, x, child)
 *         T = ["tag", ["a", -1, "b", "y", "flag", true], -2, "text", ["i", null]]
 *
 *   node  := [tag, props, child, ...]
 *   tag   := "tag"      a string: an intrinsic element
 *          | null       a fragment
 *          | hole       a component (`Comp`, `Comp.Inner`), evaluated first
 *   props := null       no attributes at all
 *          | [name, value, name, value, ...]   in source order; a name of
 *                       null is a {...spread}, its value a hole
 *   value := string | number | true | hole | node
 *   child := string | number | hole | node
 *   hole  := -1 - i     the i-th argument after the template
 *
 * A `{...}` holding one number or string literal is static and lands in the
 * template; every other expression is a hole, evaluated at the call in source
 * order (tag, attributes, children). A static number is never negative, so a
 * negative number is always a hole. The template is one object per site, made
 * when the site is parsed, sealed (read-only elements, not extensible) and
 * pushed from the function's constant pool: the same object on every
 * evaluation, so a consumer may cache what it read from it by identity. In
 * bytecode it travels as a template object, as a tagged template's strings do.
 * An element inside a `{}` is a site of its own.
 *
 * The call shape is what BLOCK(T, ...holes) can make of the template with the
 * factory's rules: props from the pairs, `children` from the children (one
 * child the value itself, several an array), a function tag called, a
 * fragment its children.
 */

typedef struct {
    int holes;
} TSJsxBlock;

/* Append `v` (owned) to the template array as a read-only element. */
static int ts_jsx_block_push(JSParseState *s, JSValue arr, uint32_t *len, JSValue v)
{
    if (JS_IsException(v))
        return -1;
    return JS_DefinePropertyValueUint32(s->ctx, arr, (*len)++, v,
                                        JS_PROP_ENUMERABLE | JS_PROP_THROW) < 0 ? -1 : 0;
}

static JSValue ts_jsx_block_hole(JSParseState *s, TSJsxBlock *b, const uint8_t *p)
{
    /* the template is the first argument and OP_call counts 16 bits */
    if (b->holes >= 65534) {
        ts_jsx_error(s, p, "too many JSX expressions in one element");
        return JS_EXCEPTION;
    }
    return JS_NewInt32(s->ctx, -1 - b->holes++);
}

static int ts_jsx_block_seal(JSParseState *s, JSValue arr)
{
    return seal_template_obj(s->ctx, arr);
}

static __exception int ts_jsx_block_element(JSParseState *s, const uint8_t **pp,
                                            int depth, TSJsxBlock *b, JSValue *pnode);

/* `{...}` in a block. *pvalue is the static literal, or the hole once the
   expression is emitted; *pempty for `{}` and a comment alone. With
   allow_spread a leading `...` is consumed (and no literal is folded, so a
   spread is always a hole); the rest of the grammar is the factory's. */
static __exception int ts_jsx_block_embedded(JSParseState *s, const uint8_t **pp,
                                             TSJsxBlock *b, BOOL allow_spread,
                                             JSValue *pvalue, BOOL *pempty)
{
    const uint8_t *open = *pp;

    *pvalue = JS_UNDEFINED;
    *pempty = FALSE;
    s->buf_ptr = open + 1;
    if (next_token(s))
        return -1;
    if (s->token.val == '}') {
        *pempty = TRUE;
        *pp = s->buf_ptr;
        return 0;
    }
    if (allow_spread) {
        if (s->token.val == TOK_ELLIPSIS && next_token(s))
            return -1;
    } else if (s->token.val == TOK_NUMBER || s->token.val == TOK_STRING) {
        /* one literal alone is static: look one token ahead for the '}' and
           re-lex the literal if the expression goes on */
        JSValue lit = JS_DupValue(s->ctx, s->token.val == TOK_NUMBER ?
                                  s->token.u.num.val : s->token.u.str.str);
        int tag = JS_VALUE_GET_TAG(lit);
        BOOL simple = tag == JS_TAG_STRING || tag == JS_TAG_FLOAT64 ||
            (tag == JS_TAG_INT && JS_VALUE_GET_INT(lit) >= 0);
        JSParsePos pos;

        js_parse_get_pos(s, &pos);
        if (next_token(s)) {
            JS_FreeValue(s->ctx, lit);
            return -1;
        }
        if (simple && s->token.val == '}') {
            *pvalue = lit;
            *pp = s->buf_ptr;
            return 0;
        }
        JS_FreeValue(s->ctx, lit);
        if (js_parse_seek_token(s, &pos))
            return -1;
    }
    if (js_parse_assign_expr(s))
        return -1;
    if (s->token.val != '}')
        return js_parse_error(s, "expecting '}' in JSX expression");
    *pvalue = ts_jsx_block_hole(s, b, open);
    if (JS_IsException(*pvalue))
        return -1;
    *pp = s->buf_ptr;
    return 0;
}

/* Children appended to `node`, up to the matching closing tag. */
static __exception int ts_jsx_block_children(JSParseState *s, const uint8_t **pp,
                                             const char *name, size_t name_len,
                                             int depth, TSJsxBlock *b,
                                             JSValue node, uint32_t *len)
{
    const uint8_t *p = *pp;
    const uint8_t *end = s->buf_end;

    for (;;) {
        if (p >= end)
            return ts_jsx_error(s, p, "unterminated JSX element");
        if (p[0] == '<' && p + 1 < end && p[1] == '/') {
            const uint8_t *q = p + 2;
            const uint8_t *nstart = q;
            while (q < end && ts_jsx_name_char(*q))
                q++;
            if (name_len != (size_t)(q - nstart) ||
                (name_len && memcmp(nstart, name, name_len) != 0))
                return ts_jsx_error(s, p, "mismatched JSX closing tag");
            q = ts_jsx_skip_space(q, end);
            if (q >= end || *q != '>')
                return ts_jsx_error(s, q, "expecting '>' in JSX closing tag");
            *pp = q + 1;
            return 0;
        }
        if (p[0] == '<') {
            JSValue child;
            if (ts_jsx_block_element(s, &p, depth + 1, b, &child))
                return -1;
            if (ts_jsx_block_push(s, node, len, child))
                return -1;
            continue;
        }
        if (p[0] == '{') {
            JSValue value;
            BOOL empty;
            if (ts_jsx_block_embedded(s, &p, b, FALSE, &value, &empty))
                return -1;
            if (!empty && ts_jsx_block_push(s, node, len, value))
                return -1;
            continue;
        }
        {
            const uint8_t *start = p;
            TSBuf text;
            int ret = 0;
            while (p < end && *p != '<' && *p != '{')
                p++;
            ts_buf_init(&text);
            ts_jsx_text(s->ctx, &text, (const char *)start, p - start);
            if (text.failed)
                ret = -1;
            else if (text.len > 0)
                ret = ts_jsx_block_push(s, node, len,
                                        JS_NewStringLen(s->ctx, text.buf, text.len));
            ts_buf_free(s->ctx, &text);
            if (ret)
                return -1;
        }
    }
}

/* Attributes as the template's props: null, or a sealed [name, value, ...]. */
static __exception int ts_jsx_block_props(JSParseState *s, const uint8_t **pp,
                                          int depth, TSJsxBlock *b, JSValue *pprops)
{
    const uint8_t *p = *pp;
    const uint8_t *end = s->buf_end;
    JSValue props;
    uint32_t len = 0;

    p = ts_jsx_skip_space(p, end);
    if (p >= end || *p == '>' || *p == '/') {
        *pprops = JS_NULL;
        *pp = p;
        return 0;
    }
    props = JS_NewArray(s->ctx);
    if (JS_IsException(props))
        return -1;

    for (;;) {
        p = ts_jsx_skip_space(p, end);
        if (p >= end) {
            ts_jsx_error(s, p, "unterminated JSX attributes");
            goto fail;
        }
        if (*p == '>' || *p == '/')
            break;
        if (*p == '{') {
            /* {...spread}: a null name, the object a hole */
            JSValue value;
            BOOL empty;
            if (ts_jsx_block_embedded(s, &p, b, TRUE, &value, &empty))
                goto fail;
            if (empty)
                continue;
            if (ts_jsx_block_push(s, props, &len, JS_NULL) ||
                ts_jsx_block_push(s, props, &len, value))
                goto fail;
            continue;
        }
        {
            const uint8_t *nstart = p;
            JSValue value;
            while (p < end && ts_jsx_name_char(*p))
                p++;
            if (p == nstart) {
                ts_jsx_error(s, p, "invalid JSX attribute name");
                goto fail;
            }
            if (ts_jsx_block_push(s, props, &len,
                                  JS_NewStringLen(s->ctx, (const char *)nstart, p - nstart)))
                goto fail;
            p = ts_jsx_skip_space(p, end);
            if (p >= end || *p != '=') {
                value = JS_TRUE;                /* bare attribute */
            } else {
                p++;
                p = ts_jsx_skip_space(p, end);
                if (p < end && (*p == '"' || *p == '\'')) {
                    int quote = *p++;
                    const uint8_t *vstart = p;
                    TSBuf val;
                    while (p < end && *p != quote)
                        p++;
                    if (p >= end) {
                        ts_jsx_error(s, p, "unterminated JSX attribute");
                        goto fail;
                    }
                    ts_buf_init(&val);
                    ts_jsx_entities(s->ctx, &val, (const char *)vstart, p - vstart);
                    p++;                        /* closing quote */
                    value = val.failed ? JS_EXCEPTION :
                        JS_NewStringLen(s->ctx, val.buf ? val.buf : "", val.len);
                    ts_buf_free(s->ctx, &val);
                } else if (p < end && *p == '{') {
                    BOOL empty;
                    if (ts_jsx_block_embedded(s, &p, b, FALSE, &value, &empty))
                        goto fail;
                    if (empty)
                        value = JS_TRUE;
                } else if (p < end && *p == '<') {
                    if (ts_jsx_block_element(s, &p, depth + 1, b, &value))
                        goto fail;
                } else {
                    ts_jsx_error(s, p, "invalid JSX attribute value");
                    goto fail;
                }
            }
            if (ts_jsx_block_push(s, props, &len, value))
                goto fail;
        }
    }
    if (ts_jsx_block_seal(s, props))
        goto fail;
    *pprops = props;
    *pp = p;
    return 0;
 fail:
    JS_FreeValue(s->ctx, props);
    return -1;
}

/* *pp points at '<'. Builds the element's node, emitting its holes in
   source order, and leaves *pp just past the element. */
static __exception int ts_jsx_block_element(JSParseState *s, const uint8_t **pp,
                                            int depth, TSJsxBlock *b, JSValue *pnode)
{
    const uint8_t *p = *pp;
    const uint8_t *end = s->buf_end;
    const uint8_t *nstart;
    size_t name_len;
    JSValue node, props;
    uint32_t len = 0;

    if (depth > TS_JSX_MAX_DEPTH)
        return ts_jsx_error(s, p, "JSX nested too deeply");
    if (p >= end || *p != '<')
        return ts_jsx_error(s, p, "expecting JSX element");
    p++;

    node = JS_NewArray(s->ctx);
    if (JS_IsException(node))
        return -1;

    if (p < end && *p == '>') {
        /* <>...</>: a null tag, no props */
        p++;
        if (ts_jsx_block_push(s, node, &len, JS_NULL) ||
            ts_jsx_block_push(s, node, &len, JS_NULL) ||
            ts_jsx_block_children(s, &p, NULL, 0, depth, b, node, &len))
            goto fail;
        goto done;
    }

    nstart = p;
    while (p < end && ts_jsx_name_char(*p))
        p++;
    name_len = p - nstart;
    if (name_len == 0) {
        ts_jsx_error(s, p, "invalid JSX tag name");
        goto fail;
    }
    if (ts_jsx_tag_is_string((const char *)nstart, name_len)) {
        if (ts_jsx_block_push(s, node, &len,
                              JS_NewStringLen(s->ctx, (const char *)nstart, name_len)))
            goto fail;
    } else {
        /* a component: the expression is the first hole of the element */
        if (ts_jsx_emit_tag(s, (const char *)nstart, name_len) ||
            ts_jsx_block_push(s, node, &len, ts_jsx_block_hole(s, b, nstart)))
            goto fail;
    }

    if (s->ts && p < end && *p == '<') {
        const uint8_t *after = ts_jsx_skip_type_args(p, end);
        if (after)
            p = after;
    }

    if (ts_jsx_block_props(s, &p, depth, b, &props) ||
        ts_jsx_block_push(s, node, &len, props))
        goto fail;

    if (p + 1 < end && p[0] == '/' && p[1] == '>') {
        p += 2;
        goto done;
    }
    if (p >= end || *p != '>') {
        ts_jsx_error(s, p, "expecting '>' in JSX element");
        goto fail;
    }
    p++;
    if (ts_jsx_block_children(s, &p, (const char *)nstart, name_len, depth, b, node, &len))
        goto fail;

 done:
    if (ts_jsx_block_seal(s, node))
        goto fail;
    *pnode = node;
    *pp = p;
    return 0;
 fail:
    JS_FreeValue(s->ctx, node);
    return -1;
}

/* A whole site: BLOCK(template, ...holes). The template's slot in the
   constant pool is taken before the scan, since the push comes before the
   holes on the stack, and filled when the element is complete. */
static __exception int ts_jsx_block_site(JSParseState *s, const uint8_t **pp)
{
    JSAtom block;
    TSJsxBlock b;
    JSValue node;
    int idx;

    block = JS_NewAtom(s->ctx, TS_JSX_BLOCK);
    if (block == JS_ATOM_NULL)
        return -1;
    ts_emit_get_var(s, block);
    JS_FreeAtom(s->ctx, block);

    idx = cpool_add(s, JS_UNDEFINED);
    if (idx < 0)
        return -1;
    emit_op(s, OP_push_const);
    emit_u32(s, idx);

    b.holes = 0;
    if (ts_jsx_block_element(s, pp, 0, &b, &node))
        return -1;
    s->cur_func->cpool[idx] = node;

    emit_op(s, OP_call);
    emit_u16(s, 1 + b.holes);
    return 0;
}

/* Entry point from js_parse_postfix_expr: the current token is '<' and JSX
   is enabled, so it starts an element. */
static __exception int ts_parse_jsx(JSParseState *s)
{
    const uint8_t *p = s->token.ptr;

    if (s->jsx_blocks ? ts_jsx_block_site(s, &p) : ts_jsx_element(s, &p, 0))
        return -1;
    s->buf_ptr = p;
    return next_token(s);
}
