#ifndef FGLSL_H
#define FGLSL_H

/*
 * fglsl.h - the FGSL shader compiler (Farcade GL Shading Language).
 *
 * Compiles a vertex shader and a fragment shader written in a GLSL-style
 * language into one FGPU shader program blob (fgpu_abi.h, "Shader
 * programs"), which SYS_GPU_CREATE_PROGRAM checks and runs. FarcadeGL's
 * fglCreateShader() calls this for you; use it directly only to
 * precompile shaders or inspect the result.
 *
 *     static fglsl_program_t prog;           // large: ~160 KiB, keep it off the stack
 *     if (fglsl_compile(&prog, vertex_src, fragment_src) != 0)
 *         print(prog.error);
 *     // prog.words / prog.size: the blob; prog.uniforms: where each uniform lives
 *
 * The language is documented in docs/fgsl.md. In short: GLSL ES 1.0 /
 * GLSL 1.30 syntax with float, vec2-4, mat4, sampler2D (int and bool are
 * floats), uniforms (also arrays), attributes, varyings, user functions
 * (inlined, no recursion), if/else, ?:, for loops with constant bounds
 * (unrolled), discard, and the common built-in functions.
 *
 * Freestanding: no libc, no malloc. Works on the host too.
 */

#include <stdint.h>
#include <stddef.h>
#include "fgpu_abi.h"

#define FGLSL_MAX_UNIFORMS 48           // names, including samplers
#define FGLSL_NAME_MAX 32
#define FGLSL_MAX_BLOB_WORDS (sizeof(fgpu_shader_header_t) / 4 + \
    2 * FGPU_SHADER_MAX_INSTRUCTIONS * FGPU_SHADER_INSTRUCTION_WORDS + 4 * FGPU_SHADER_MAX_CONSTANTS)

// Types
#define FGLSL_VOID    0
#define FGLSL_FLOAT   1
#define FGLSL_VEC2    2
#define FGLSL_VEC3    3
#define FGLSL_VEC4    4
#define FGLSL_MAT4    16
#define FGLSL_SAMPLER 32

typedef struct {
    char name[FGLSL_NAME_MAX];
    int type;                   // FGLSL_*
    uint32_t array;             // array length, 0 when not an array
    uint32_t reg;               // first uniform register, or the texture unit for samplers
    uint32_t regs;              // registers used (mat4 = 4 per element)
} fglsl_uniform_t;

typedef struct {
    char name[FGLSL_NAME_MAX];
    int type;
    uint32_t location;          // vertex layout attribute
} fglsl_attrib_t;

/* --------------------------------------------------- compiler internals */

typedef struct {
    int file, index;
    uint8_t sw[4];
    int type;                   // FGLSL_*
    int count;                  // array length (0 = not an array)
    int neg, abs;
    int lvalue;
    int fresh;                  // a temp written by the last instruction
} fglsl__val;

typedef struct {
    char name[FGLSL_NAME_MAX];
    int scope;
    fglsl__val v;
} fglsl__sym;

typedef struct {
    const char* src;
    uint32_t pos;
    int line;
    int tok;
    float num;
    char id[FGLSL_NAME_MAX];
} fglsl__lex;

typedef struct {
    char name[FGLSL_NAME_MAX];
    int ret;
    int nparams;
    char pname[8][FGLSL_NAME_MAX];
    int ptype[8];
    fglsl__lex body;            // at the '{'
} fglsl__func;

typedef struct {
    char name[FGLSL_NAME_MAX];
    int type;
} fglsl__varying;

#define FGLSL__MAX_SYMS 192
#define FGLSL__MAX_FUNCS 32
#define FGLSL__MAX_PATCH 64

typedef struct {
    /* ---- results ---- */
    uint32_t words[FGLSL_MAX_BLOB_WORDS];
    uint32_t size;                      // bytes, 0 on error
    fglsl_uniform_t uniforms[FGLSL_MAX_UNIFORMS];
    uint32_t uniform_count;
    fglsl_attrib_t attribs[FGPU_MAX_ATTRIBS];
    uint32_t attrib_count;
    char error[192];                    // "" on success

    /* ---- internals ---- */
    int err;
    int fragment;                       // stage being compiled
    fglsl__lex lx;
    uint32_t code[FGPU_SHADER_MAX_INSTRUCTIONS * FGPU_SHADER_INSTRUCTION_WORDS];
    uint32_t count;                     // instructions in this stage
    uint32_t label_pc;                  // a jump lands at this instruction
    float pool[FGPU_SHADER_MAX_CONSTANTS][4];
    int pool_fill[FGPU_SHADER_MAX_CONSTANTS];
    uint32_t pool_count;
    fglsl__sym syms[FGLSL__MAX_SYMS];
    int nsyms, scope;
    int ntemp, max_temp;
    fglsl__func funcs[FGLSL__MAX_FUNCS];
    int nfuncs;
    fglsl__lex main_body;
    int has_main;
    fglsl__varying varyings[FGPU_SHADER_MAX_VARYINGS];
    int nvaryings;
    uint32_t next_uniform_reg, next_unit, attrib_mask, texture_mask, flags;
    // return / break / continue jump lists
    uint32_t* ret_list; int* ret_n; fglsl__val ret_val; int ret_type;
    uint32_t* brk_list; int* brk_n;
    uint32_t* cont_list; int* cont_n;
    int inline_depth;
} fglsl_program_t;

/* ======================================================================
 *  Implementation (everything static)
 * ====================================================================== */

#define FGLSL__API static __attribute__((unused))

enum {
    FGLSL__TK_EOF = 256, FGLSL__TK_NUM, FGLSL__TK_ID, FGLSL__TK_EQ, FGLSL__TK_NE,
    FGLSL__TK_LE, FGLSL__TK_GE, FGLSL__TK_AND, FGLSL__TK_OR, FGLSL__TK_ADDEQ,
    FGLSL__TK_SUBEQ, FGLSL__TK_MULEQ, FGLSL__TK_DIVEQ, FGLSL__TK_INC, FGLSL__TK_DEC
};

static int fglsl__streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void fglsl__strcpy(char* d, const char* s, int cap) {
    int i = 0;
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static void fglsl__append(char* d, int cap, const char* s) {
    int n = 0;
    while (d[n]) n++;
    while (*s && n < cap - 1) d[n++] = *s++;
    d[n] = '\0';
}

static void fglsl__error(fglsl_program_t* c, const char* msg, const char* detail) {
    if (c->err) return;
    c->err = 1;
    char num[12];
    int line = c->lx.line, n = 0;
    char t[12];
    do { t[n++] = (char)('0' + line % 10); line /= 10; } while (line && n < 11);
    int k = 0;
    while (n) num[k++] = t[--n];
    num[k] = '\0';
    c->error[0] = '\0';
    fglsl__append(c->error, sizeof c->error, c->fragment ? "fragment shader line " : "vertex shader line ");
    fglsl__append(c->error, sizeof c->error, num);
    fglsl__append(c->error, sizeof c->error, ": ");
    fglsl__append(c->error, sizeof c->error, msg);
    if (detail) {
        fglsl__append(c->error, sizeof c->error, " '");
        fglsl__append(c->error, sizeof c->error, detail);
        fglsl__append(c->error, sizeof c->error, "'");
    }
}

/* ------------------------------------------------------------- lexer */

static int fglsl__isid(char ch, int first) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' ||
           (!first && ch >= '0' && ch <= '9');
}

static void fglsl__next(fglsl_program_t* c) {
    fglsl__lex* l = &c->lx;
    const char* s = l->src;
    for (;;) {
        char ch = s[l->pos];
        if (ch == '\n') { l->line++; l->pos++; continue; }
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\f' || ch == '\v') { l->pos++; continue; }
        if (ch == '/' && s[l->pos + 1] == '/') {
            while (s[l->pos] && s[l->pos] != '\n') l->pos++;
            continue;
        }
        if (ch == '/' && s[l->pos + 1] == '*') {
            l->pos += 2;
            while (s[l->pos] && !(s[l->pos] == '*' && s[l->pos + 1] == '/')) {
                if (s[l->pos] == '\n') l->line++;
                l->pos++;
            }
            if (s[l->pos]) l->pos += 2;
            continue;
        }
        if (ch == '#') {            // #version, #extension...: ignored
            while (s[l->pos] && s[l->pos] != '\n') l->pos++;
            continue;
        }
        break;
    }
    char ch = s[l->pos];
    if (!ch) { l->tok = FGLSL__TK_EOF; return; }
    if (fglsl__isid(ch, 1)) {
        int n = 0;
        while (fglsl__isid(s[l->pos], 0)) {
            if (n < FGLSL_NAME_MAX - 1) l->id[n++] = s[l->pos];
            l->pos++;
        }
        l->id[n] = '\0';
        l->tok = FGLSL__TK_ID;
        return;
    }
    if ((ch >= '0' && ch <= '9') || (ch == '.' && s[l->pos + 1] >= '0' && s[l->pos + 1] <= '9')) {
        double v = 0.0, scale = 1.0;
        while (s[l->pos] >= '0' && s[l->pos] <= '9') v = v * 10.0 + (s[l->pos++] - '0');
        if (s[l->pos] == '.') {
            l->pos++;
            while (s[l->pos] >= '0' && s[l->pos] <= '9') { scale /= 10.0; v += (s[l->pos++] - '0') * scale; }
        }
        if (s[l->pos] == 'e' || s[l->pos] == 'E') {
            uint32_t save = l->pos;
            l->pos++;
            int neg = 0, e = 0, digits = 0;
            if (s[l->pos] == '+' || s[l->pos] == '-') neg = s[l->pos++] == '-';
            while (s[l->pos] >= '0' && s[l->pos] <= '9') { e = e * 10 + (s[l->pos++] - '0'); digits++; if (e > 60) e = 60; }
            if (!digits) l->pos = save;
            while (e-- > 0) v = neg ? v / 10.0 : v * 10.0;
        }
        if (s[l->pos] == 'f' || s[l->pos] == 'F' || s[l->pos] == 'u' || s[l->pos] == 'U') l->pos++;
        l->num = (float)v;
        l->tok = FGLSL__TK_NUM;
        return;
    }
    char d = s[l->pos + 1];
    l->pos++;
    l->tok = ch;
    switch (ch) {
        case '=': if (d == '=') { l->pos++; l->tok = FGLSL__TK_EQ; } break;
        case '!': if (d == '=') { l->pos++; l->tok = FGLSL__TK_NE; } break;
        case '<': if (d == '=') { l->pos++; l->tok = FGLSL__TK_LE; } break;
        case '>': if (d == '=') { l->pos++; l->tok = FGLSL__TK_GE; } break;
        case '&': if (d == '&') { l->pos++; l->tok = FGLSL__TK_AND; } break;
        case '|': if (d == '|') { l->pos++; l->tok = FGLSL__TK_OR; } break;
        case '+': if (d == '=') { l->pos++; l->tok = FGLSL__TK_ADDEQ; } else if (d == '+') { l->pos++; l->tok = FGLSL__TK_INC; } break;
        case '-': if (d == '=') { l->pos++; l->tok = FGLSL__TK_SUBEQ; } else if (d == '-') { l->pos++; l->tok = FGLSL__TK_DEC; } break;
        case '*': if (d == '=') { l->pos++; l->tok = FGLSL__TK_MULEQ; } break;
        case '/': if (d == '=') { l->pos++; l->tok = FGLSL__TK_DIVEQ; } break;
        default: break;
    }
}

static int fglsl__is(fglsl_program_t* c, int tok) { return c->lx.tok == tok; }
static int fglsl__isword(fglsl_program_t* c, const char* w) {
    return c->lx.tok == FGLSL__TK_ID && fglsl__streq(c->lx.id, w);
}
static int fglsl__accept(fglsl_program_t* c, int tok) {
    if (c->lx.tok != tok) return 0;
    fglsl__next(c);
    return 1;
}
static void fglsl__expect(fglsl_program_t* c, int tok) {
    if (c->err) return;
    if (!fglsl__accept(c, tok)) {
        char t[2] = { (char)tok, 0 };
        fglsl__error(c, "expected", tok < 256 ? t : "token");
    }
}

static void fglsl__skip(fglsl_program_t* c);

// Skips "( ... )".
static void fglsl__skip_parens(fglsl_program_t* c) {
    if (!fglsl__is(c, '(')) { fglsl__error(c, "expected '('", 0); return; }
    int depth = 0;
    do {
        if (fglsl__is(c, FGLSL__TK_EOF)) { fglsl__error(c, "unexpected end of shader", 0); return; }
        if (fglsl__is(c, '(')) depth++;
        else if (fglsl__is(c, ')')) depth--;
        fglsl__next(c);
    } while (depth > 0);
}

// Skips a statement or a { block } without compiling it.
static void fglsl__skip(fglsl_program_t* c) {
    if (fglsl__isword(c, "if")) {
        fglsl__next(c);
        fglsl__skip_parens(c);
        fglsl__skip(c);
        if (fglsl__isword(c, "else")) { fglsl__next(c); fglsl__skip(c); }
        return;
    }
    if (fglsl__isword(c, "for")) {
        fglsl__next(c);
        fglsl__skip_parens(c);
        fglsl__skip(c);
        return;
    }
    int depth = 0;
    for (;;) {
        if (fglsl__is(c, FGLSL__TK_EOF)) { fglsl__error(c, "unexpected end of shader", 0); return; }
        int tok = c->lx.tok;
        fglsl__next(c);
        if (tok == '{' || tok == '(') depth++;
        else if (tok == '}' || tok == ')') {
            depth--;
            if (depth <= 0 && tok == '}') return;
        } else if (tok == ';' && depth == 0) {
            return;
        }
    }
}

/* ------------------------------------------------------------- types */

static int fglsl__type_of(const char* w) {
    if (fglsl__streq(w, "void")) return FGLSL_VOID;
    if (fglsl__streq(w, "float") || fglsl__streq(w, "int") || fglsl__streq(w, "bool") ||
        fglsl__streq(w, "uint")) return FGLSL_FLOAT;
    if (fglsl__streq(w, "vec2") || fglsl__streq(w, "ivec2") || fglsl__streq(w, "bvec2")) return FGLSL_VEC2;
    if (fglsl__streq(w, "vec3") || fglsl__streq(w, "ivec3") || fglsl__streq(w, "bvec3")) return FGLSL_VEC3;
    if (fglsl__streq(w, "vec4") || fglsl__streq(w, "ivec4") || fglsl__streq(w, "bvec4")) return FGLSL_VEC4;
    if (fglsl__streq(w, "mat4")) return FGLSL_MAT4;
    if (fglsl__streq(w, "sampler2D")) return FGLSL_SAMPLER;
    return -1;
}

static int fglsl__comps(int type) { return type >= 1 && type <= 4 ? type : (type == FGLSL_MAT4 ? 16 : 0); }
static int fglsl__regs(int type) { return type == FGLSL_MAT4 ? 4 : 1; }

/* ------------------------------------------------------- values, code */

static fglsl__val fglsl__mkval(int file, int index, int type) {
    fglsl__val v;
    v.file = file; v.index = index; v.type = type; v.count = 0;
    v.neg = v.abs = v.lvalue = v.fresh = 0;
    for (int i = 0; i < 4; i++) v.sw[i] = (uint8_t)i;
    return v;
}

static int fglsl__temp(fglsl_program_t* c, int regs) {
    int t = c->ntemp;
    c->ntemp += regs;
    if (c->ntemp > (int)FGPU_SHADER_MAX_TEMPS) {
        fglsl__error(c, "shader too complex (out of temporary registers)", 0);
        c->ntemp = 0;
        return 0;
    }
    if (c->ntemp > c->max_temp) c->max_temp = c->ntemp;
    return t;
}

static fglsl__val fglsl__newtemp(fglsl_program_t* c, int type) {
    fglsl__val v = fglsl__mkval(FGPU_FILE_TEMP, fglsl__temp(c, fglsl__regs(type)), type);
    v.fresh = 1;
    return v;
}

// Swizzle that repeats the val's scalar to all lanes (or pads a vecN).
static fglsl__val fglsl__bcast(fglsl__val v) {
    int n = fglsl__comps(v.type);
    if (n >= 1 && n < 4)
        for (int i = n; i < 4; i++) v.sw[i] = v.sw[n - 1];
    return v;
}

static uint32_t fglsl__src(fglsl__val v) {
    if (v.file == FGPU_FILE_NONE) return 0;
    v = fglsl__bcast(v);
    uint32_t w = FGPU_SRC(v.file, v.index, FGPU_SWIZZLE(v.sw[0], v.sw[1], v.sw[2], v.sw[3]));
    if (v.abs) w |= FGPU_SRC_ABS;
    if (v.neg) w |= FGPU_SRC_NEGATE;
    return w;
}

static uint32_t fglsl__emit(fglsl_program_t* c, uint32_t op, uint32_t dst, uint32_t a, uint32_t b, uint32_t cc) {
    if (c->err) return 0;
    if (c->count >= FGPU_SHADER_MAX_INSTRUCTIONS) {
        fglsl__error(c, "shader too long", 0);
        return 0;
    }
    uint32_t* in = c->code + (size_t)c->count * FGPU_SHADER_INSTRUCTION_WORDS;
    in[0] = op; in[1] = dst; in[2] = a; in[3] = b; in[4] = cc;
    return c->count++;
}

static uint32_t fglsl__dst(fglsl__val d, int comps) {
    return FGPU_DST(d.file, d.index, comps >= 4 ? 0xF : ((1u << comps) - 1u));
}

// Places pending jumps here.
static void fglsl__patch(fglsl_program_t* c, uint32_t* list, int n) {
    // A jump that is the last instruction would land on the next one:
    // drop it (e.g. "return" at the end of an inlined function).
    for (;;) {
        int found = -1;
        for (int i = 0; i < n; i++)
            if (c->count && list[i] == c->count - 1 &&
                c->code[(size_t)(c->count - 1) * FGPU_SHADER_INSTRUCTION_WORDS] == FGPU_SOP_JMP) found = i;
        if (found < 0 || c->label_pc == c->count) break;
        c->count--;
        list[found] = list[--n];
    }
    for (int i = 0; i < n; i++) c->code[(size_t)list[i] * FGPU_SHADER_INSTRUCTION_WORDS + 1] = c->count;
    c->label_pc = c->count;
}

/* -------------------------------------------------------- constants */

static fglsl__val fglsl__const(fglsl_program_t* c, const float* vals, int n) {
    if (n == 1) {
        for (uint32_t r = 0; r < c->pool_count; r++)
            for (int k = 0; k < c->pool_fill[r]; k++)
                if (c->pool[r][k] == vals[0]) {
                    fglsl__val v = fglsl__mkval(FGPU_FILE_CONST, (int)r, FGLSL_FLOAT);
                    v.sw[0] = v.sw[1] = v.sw[2] = v.sw[3] = (uint8_t)k;
                    return v;
                }
        if (c->pool_count && c->pool_fill[c->pool_count - 1] < 4) {
            uint32_t r = c->pool_count - 1;
            int k = c->pool_fill[r]++;
            c->pool[r][k] = vals[0];
            fglsl__val v = fglsl__mkval(FGPU_FILE_CONST, (int)r, FGLSL_FLOAT);
            v.sw[0] = v.sw[1] = v.sw[2] = v.sw[3] = (uint8_t)k;
            return v;
        }
    } else {
        for (uint32_t r = 0; r < c->pool_count; r++) {
            if (c->pool_fill[r] < n) continue;
            int same = 1;
            for (int k = 0; k < n; k++) same &= c->pool[r][k] == vals[k];
            if (same) return fglsl__mkval(FGPU_FILE_CONST, (int)r, n);
        }
    }
    if (c->pool_count >= FGPU_SHADER_MAX_CONSTANTS) {
        fglsl__error(c, "too many constants", 0);
        return fglsl__mkval(FGPU_FILE_CONST, 0, n);
    }
    uint32_t r = c->pool_count++;
    for (int k = 0; k < 4; k++) c->pool[r][k] = k < n ? vals[k] : 0.0f;
    c->pool_fill[r] = n;
    fglsl__val v = fglsl__mkval(FGPU_FILE_CONST, (int)r, n);
    if (n == 1) v.sw[1] = v.sw[2] = v.sw[3] = 0;
    return v;
}

static fglsl__val fglsl__scalar(fglsl_program_t* c, float x) { return fglsl__const(c, &x, 1); }

static int fglsl__is_const(fglsl__val v) { return v.file == FGPU_FILE_CONST && v.type >= 1 && v.type <= 4; }

static void fglsl__const_value(fglsl_program_t* c, fglsl__val v, float out[4]) {
    v = fglsl__bcast(v);
    for (int i = 0; i < 4; i++) {
        float x = c->pool[v.index][v.sw[i]];
        if (v.abs && x < 0) x = -x;
        if (v.neg) x = -x;
        out[i] = x;
    }
}

/* ------------------------------------------------------------ symbols */

static fglsl__sym* fglsl__find(fglsl_program_t* c, const char* name) {
    for (int i = c->nsyms - 1; i >= 0; i--)
        if (fglsl__streq(c->syms[i].name, name)) return &c->syms[i];
    return 0;
}

static void fglsl__declare(fglsl_program_t* c, const char* name, fglsl__val v) {
    for (int i = c->nsyms - 1; i >= 0 && c->syms[i].scope == c->scope; i--)
        if (fglsl__streq(c->syms[i].name, name)) { fglsl__error(c, "already declared:", name); return; }
    if (c->nsyms >= FGLSL__MAX_SYMS) { fglsl__error(c, "too many variables", 0); return; }
    fglsl__sym* s = &c->syms[c->nsyms++];
    fglsl__strcpy(s->name, name, FGLSL_NAME_MAX);
    s->scope = c->scope;
    s->v = v;
    s->v.fresh = 0;
}

/* ------------------------------------------------------ expressions */

static fglsl__val fglsl__expr(fglsl_program_t* c);
static fglsl__val fglsl__ternary(fglsl_program_t* c);
static void fglsl__statement(fglsl_program_t* c);
static void fglsl__block(fglsl_program_t* c);

static fglsl__val fglsl__none(void) { return fglsl__mkval(FGPU_FILE_NONE, 0, FGLSL_FLOAT); }

// Writes `v` into lvalue `lv` (component by component, honouring swizzles).
static void fglsl__store(fglsl_program_t* c, fglsl__val lv, fglsl__val v) {
    if (c->err) return;
    if (!lv.lvalue) { fglsl__error(c, "cannot assign to this expression", 0); return; }
    int n = fglsl__comps(lv.type);
    if (v.type == FGLSL_FLOAT && n >= 1 && n <= 4) v = fglsl__bcast(v);
    else if (v.type != lv.type) { fglsl__error(c, "type mismatch in assignment", 0); return; }
    if (lv.type == FGLSL_MAT4) {
        for (int i = 0; i < 4; i++) {
            fglsl__val col = v, d = lv;
            col.index += i; col.type = FGLSL_VEC4;
            d.index += i; d.type = FGLSL_VEC4;
            fglsl__emit(c, FGPU_SOP_MOV, fglsl__dst(d, 4), fglsl__src(col), 0, 0);
        }
        return;
    }
    uint32_t mask = 0, needed = 0;
    uint8_t sw[4] = { 0, 0, 0, 0 };
    int identity = 1;
    for (int i = 0; i < n; i++) {
        needed |= 1u << v.sw[i];
        int lane = lv.sw[i];
        if (mask & (1u << lane)) { fglsl__error(c, "a component is written twice", 0); return; }
        mask |= 1u << lane;
        sw[lane] = v.sw[i];
        if (v.sw[i] != lv.sw[i]) identity = 0;
    }
    // Write the result of the previous instruction straight into place.
    if (v.fresh && v.file == FGPU_FILE_TEMP && !v.neg && !v.abs && identity &&
        c->count > 0 && c->label_pc != c->count) {
        uint32_t* last = c->code + (size_t)(c->count - 1) * FGPU_SHADER_INSTRUCTION_WORDS;
        uint32_t op = last[0];
        if (op != FGPU_SOP_JMP && op != FGPU_SOP_JZ && op != FGPU_SOP_KILL && op != FGPU_SOP_NOP &&
            FGPU_OPERAND_FILE(last[1]) == FGPU_FILE_TEMP && (int)FGPU_OPERAND_INDEX(last[1]) == v.index &&
            (FGPU_DST_MASK(last[1]) & needed) == needed) {
            last[1] = FGPU_DST(lv.file, lv.index, mask);
            return;
        }
    }
    fglsl__val s = v;
    for (int i = 0; i < 4; i++) s.sw[i] = sw[i];
    s.type = FGLSL_VEC4;
    fglsl__emit(c, FGPU_SOP_MOV, FGPU_DST(lv.file, lv.index, mask), fglsl__src(s), 0, 0);
}

static fglsl__val fglsl__op1(fglsl_program_t* c, uint32_t op, fglsl__val a, int type) {
    fglsl__val r = fglsl__newtemp(c, type);
    fglsl__emit(c, op, fglsl__dst(r, fglsl__comps(type)), fglsl__src(a), 0, 0);
    return r;
}

static fglsl__val fglsl__op3(fglsl_program_t* c, uint32_t op, fglsl__val a, fglsl__val b, fglsl__val cc, int type) {
    fglsl__val r = fglsl__newtemp(c, type);
    fglsl__emit(c, op, fglsl__dst(r, fglsl__comps(type)), fglsl__src(a), fglsl__src(b), fglsl__src(cc));
    return r;
}

static fglsl__val fglsl__op2(fglsl_program_t* c, uint32_t op, fglsl__val a, fglsl__val b, int type) {
    return fglsl__op3(c, op, a, b, fglsl__none(), type);
}

// Result type of a component-wise operation on a and b (scalars broadcast).
static int fglsl__wide(fglsl_program_t* c, fglsl__val a, fglsl__val b) {
    if (a.type == FGLSL_FLOAT) return b.type;
    if (b.type == FGLSL_FLOAT || a.type == b.type) return a.type;
    fglsl__error(c, "operands have different sizes", 0);
    return a.type;
}

static int fglsl__vec_only(fglsl_program_t* c, fglsl__val v) {
    if (v.type < 1 || v.type > 4 || v.count) { fglsl__error(c, "expected a float or vector", 0); return 0; }
    return 1;
}

static fglsl__val fglsl__mat_vec(fglsl_program_t* c, fglsl__val m, fglsl__val v) {
    // r = col0 * v.x + col1 * v.y + col2 * v.z + col3 * v.w
    fglsl__val r = fglsl__newtemp(c, FGLSL_VEC4);
    for (int i = 0; i < 4; i++) {
        fglsl__val col = m, lane = v;
        col.index += i; col.type = FGLSL_VEC4;
        lane.sw[0] = lane.sw[1] = lane.sw[2] = lane.sw[3] = v.sw[i];
        lane.type = FGLSL_FLOAT;
        if (i == 0) fglsl__emit(c, FGPU_SOP_MUL, fglsl__dst(r, 4), fglsl__src(col), fglsl__src(lane), 0);
        else fglsl__emit(c, FGPU_SOP_MAD, fglsl__dst(r, 4), fglsl__src(col), fglsl__src(lane), fglsl__src(r));
    }
    return r;
}

static fglsl__val fglsl__binary(fglsl_program_t* c, int tok, fglsl__val a, fglsl__val b) {
    if (c->err) return a;
    if (a.count || b.count) { fglsl__error(c, "arrays cannot be used in arithmetic", 0); return a; }
    if (tok == '*' && a.type == FGLSL_MAT4) {
        if (b.type == FGLSL_VEC4) return fglsl__mat_vec(c, a, b);
        if (b.type == FGLSL_MAT4) {
            fglsl__val r = fglsl__newtemp(c, FGLSL_MAT4);
            for (int j = 0; j < 4; j++) {
                fglsl__val col = b;
                col.index += j; col.type = FGLSL_VEC4;
                int mark = c->ntemp;
                fglsl__val t = fglsl__mat_vec(c, a, col);
                fglsl__val d = r;
                d.index += j; d.type = FGLSL_VEC4; d.lvalue = 1;
                fglsl__store(c, d, t);
                c->ntemp = mark;
            }
            r.fresh = 0;
            return r;
        }
        fglsl__error(c, "mat4 can only be multiplied by vec4 or mat4", 0);
        return a;
    }
    if (tok == '*' && b.type == FGLSL_MAT4) {
        if (a.type != FGLSL_VEC4) { fglsl__error(c, "only vec4 * mat4 is supported", 0); return a; }
        fglsl__val r = fglsl__newtemp(c, FGLSL_VEC4);
        for (int i = 0; i < 4; i++) {
            fglsl__val col = b;
            col.index += i; col.type = FGLSL_VEC4;
            fglsl__emit(c, FGPU_SOP_DP4, FGPU_DST(FGPU_FILE_TEMP, r.index, 1u << i), fglsl__src(a), fglsl__src(col), 0);
        }
        return r;
    }
    if (!fglsl__vec_only(c, a) || !fglsl__vec_only(c, b)) return a;
    int type = fglsl__wide(c, a, b);
    if (fglsl__is_const(a) && fglsl__is_const(b) && (tok == '+' || tok == '-' || tok == '*' || tok == '/')) {
        float x[4], y[4], r[4];
        fglsl__const_value(c, a, x);
        fglsl__const_value(c, b, y);
        for (int i = 0; i < 4; i++)
            r[i] = tok == '+' ? x[i] + y[i] : tok == '-' ? x[i] - y[i] : tok == '*' ? x[i] * y[i] :
                   (y[i] != 0.0f ? x[i] / y[i] : 0.0f);
        return fglsl__const(c, r, type);
    }
    switch (tok) {
        case '+': return fglsl__op2(c, FGPU_SOP_ADD, a, b, type);
        case '-': return fglsl__op2(c, FGPU_SOP_SUB, a, b, type);
        case '*': return fglsl__op2(c, FGPU_SOP_MUL, a, b, type);
        case '/': return fglsl__op2(c, FGPU_SOP_DIV, a, b, type);
        default: break;
    }
    if (type != FGLSL_FLOAT) { fglsl__error(c, "comparisons and logic work on scalars only", 0); return a; }
    switch (tok) {
        case '<': return fglsl__op2(c, FGPU_SOP_SLT, a, b, type);
        case '>': return fglsl__op2(c, FGPU_SOP_SLT, b, a, type);
        case FGLSL__TK_LE: return fglsl__op2(c, FGPU_SOP_SLE, a, b, type);
        case FGLSL__TK_GE: return fglsl__op2(c, FGPU_SOP_SLE, b, a, type);
        case FGLSL__TK_EQ: return fglsl__op2(c, FGPU_SOP_SEQ, a, b, type);
        case FGLSL__TK_NE: return fglsl__op2(c, FGPU_SOP_SNE, a, b, type);
        case FGLSL__TK_AND: return fglsl__op2(c, FGPU_SOP_MUL, a, b, type);
        case FGLSL__TK_OR: return fglsl__op2(c, FGPU_SOP_MAX, a, b, type);
        default: break;
    }
    fglsl__error(c, "unknown operator", 0);
    return a;
}

static float fglsl__const_int(fglsl_program_t* c, fglsl__val v, const char* what) {
    if (!fglsl__is_const(v) || v.type != FGLSL_FLOAT) {
        fglsl__error(c, "must be a constant:", what);
        return 0.0f;
    }
    float x[4];
    fglsl__const_value(c, v, x);
    return x[0];
}

// Applies one [index] or .swizzle to v.
static fglsl__val fglsl__postfix(fglsl_program_t* c, fglsl__val v) {
    for (;;) {
        if (c->err) return v;
        if (fglsl__accept(c, '[')) {
            fglsl__val ix = fglsl__expr(c);
            fglsl__expect(c, ']');
            int i = (int)fglsl__const_int(c, ix, "index (use a literal or a loop counter)");
            if (v.count) {
                if (i < 0 || i >= v.count) { fglsl__error(c, "array index out of range", 0); return v; }
                v.index += i * fglsl__regs(v.type);
                v.count = 0;
            } else if (v.type == FGLSL_MAT4) {
                if (i < 0 || i > 3) { fglsl__error(c, "matrix column out of range", 0); return v; }
                v.index += i;
                v.type = FGLSL_VEC4;
            } else if (v.type >= 2 && v.type <= 4) {
                if (i < 0 || i >= v.type) { fglsl__error(c, "vector index out of range", 0); return v; }
                v.sw[0] = v.sw[1] = v.sw[2] = v.sw[3] = v.sw[i];
                v.type = FGLSL_FLOAT;
            } else {
                fglsl__error(c, "cannot index this value", 0);
            }
            continue;
        }
        if (fglsl__accept(c, '.')) {
            if (!fglsl__is(c, FGLSL__TK_ID)) { fglsl__error(c, "expected a swizzle", 0); return v; }
            const char* s = c->lx.id;
            int n = 0;
            uint8_t sw[4];
            int dup = 0;
            uint32_t seen = 0;
            if (v.type < 1 || v.type > 4 || v.count) { fglsl__error(c, "cannot swizzle", s); return v; }
            for (; s[n]; n++) {
                int k;
                switch (s[n]) {
                    case 'x': case 'r': case 's': k = 0; break;
                    case 'y': case 'g': case 't': k = 1; break;
                    case 'z': case 'b': case 'p': k = 2; break;
                    case 'w': case 'a': case 'q': k = 3; break;
                    default: k = 9; break;
                }
                if (n >= 4 || k >= v.type) { fglsl__error(c, "bad swizzle", c->lx.id); return v; }
                if (seen & (1u << k)) dup = 1;
                seen |= 1u << k;
                sw[n] = v.sw[k];
            }
            fglsl__next(c);
            for (int i = 0; i < n; i++) v.sw[i] = sw[i];
            v.type = n;
            if (dup) v.lvalue = 0;
            continue;
        }
        return v;
    }
}

static int fglsl__argcount(int n, int want, fglsl_program_t* c, const char* name) {
    if (n != want) { fglsl__error(c, "wrong number of arguments to", name); return 0; }
    return 1;
}

static fglsl__val fglsl__call_user(fglsl_program_t* c, fglsl__func* f, fglsl__val* args, int n);

static fglsl__val fglsl__constructor(fglsl_program_t* c, int type, fglsl__val* args, int n) {
    int want = fglsl__comps(type);
    if (type == FGLSL_MAT4) {
        if (n == 4) {
            fglsl__val r = fglsl__newtemp(c, FGLSL_MAT4);
            for (int i = 0; i < 4; i++) {
                if (args[i].type != FGLSL_VEC4) { fglsl__error(c, "mat4() takes four vec4 columns", 0); return r; }
                fglsl__val d = r;
                d.index += i; d.type = FGLSL_VEC4; d.lvalue = 1;
                fglsl__store(c, d, args[i]);
            }
            r.fresh = 0;
            return r;
        }
        if (n == 1 && args[0].type == FGLSL_FLOAT) {   // diagonal
            fglsl__val r = fglsl__newtemp(c, FGLSL_MAT4);
            fglsl__val zero = fglsl__scalar(c, 0.0f);
            for (int i = 0; i < 4; i++) {
                fglsl__val d = r;
                d.index += i; d.type = FGLSL_VEC4; d.lvalue = 1;
                fglsl__store(c, d, zero);
                d.type = FGLSL_FLOAT;
                d.sw[0] = (uint8_t)i;
                fglsl__store(c, d, args[0]);
            }
            r.fresh = 0;
            return r;
        }
        fglsl__error(c, "mat4() takes four vec4 columns or one float", 0);
        return fglsl__none();
    }
    if (n == 1 && args[0].type == type) { fglsl__val v = args[0]; v.lvalue = 0; return v; }
    if (n == 1 && args[0].type == FGLSL_FLOAT) {    // vecN(x): all lanes
        fglsl__val v = fglsl__bcast(args[0]);
        v.type = type;
        v.lvalue = 0;
        return v;
    }
    if (n == 1 && args[0].type >= 1 && args[0].type <= 4 && args[0].type > type) {  // vec3(v4)
        fglsl__val v = args[0];
        v.type = type;
        v.lvalue = 0;
        return v;
    }
    // Concatenate components.
    int total = 0, all_const = 1;
    for (int i = 0; i < n; i++) {
        if (!fglsl__vec_only(c, args[i])) return fglsl__none();
        total += args[i].type;
        all_const &= fglsl__is_const(args[i]);
    }
    if (total < want) { fglsl__error(c, "not enough components for constructor", 0); return fglsl__none(); }
    if (all_const) {
        float vals[4];
        int k = 0;
        for (int i = 0; i < n && k < want; i++) {
            float x[4];
            fglsl__const_value(c, args[i], x);
            for (int j = 0; j < args[i].type && k < want; j++) vals[k++] = x[j];
        }
        return fglsl__const(c, vals, want);
    }
    fglsl__val r = fglsl__newtemp(c, type);
    int k = 0;
    for (int i = 0; i < n && k < want; i++) {
        fglsl__val d = r;
        int take = args[i].type;
        if (take > want - k) take = want - k;
        d.type = take;
        d.lvalue = 1;
        for (int j = 0; j < take; j++) d.sw[j] = (uint8_t)(k + j);
        fglsl__val s = args[i];
        s.type = take;
        s.fresh = 0;
        fglsl__store(c, d, s);
        k += take;
    }
    r.fresh = 0;
    return r;
}

static fglsl__val fglsl__builtin(fglsl_program_t* c, const char* name, fglsl__val* a, int n, int* found) {
    *found = 1;
    for (int i = 0; i < n; i++)
        if (a[i].type != FGLSL_SAMPLER && !fglsl__vec_only(c, a[i])) return fglsl__none();
    int t0 = n > 0 ? a[0].type : FGLSL_FLOAT;
#define FGLSL__ONE(fname, op) \
    if (fglsl__streq(name, fname)) { if (!fglsl__argcount(n, 1, c, name)) return a[0]; return fglsl__op1(c, op, a[0], t0); }
    FGLSL__ONE("sin", FGPU_SOP_SIN)
    FGLSL__ONE("cos", FGPU_SOP_COS)
    FGLSL__ONE("exp2", FGPU_SOP_EXP2)
    FGLSL__ONE("log2", FGPU_SOP_LOG2)
    FGLSL__ONE("sqrt", FGPU_SOP_SQRT)
    FGLSL__ONE("inversesqrt", FGPU_SOP_RSQ)
    FGLSL__ONE("floor", FGPU_SOP_FLOOR)
    FGLSL__ONE("fract", FGPU_SOP_FRACT)
    FGLSL__ONE("sign", FGPU_SOP_SIGN)
#undef FGLSL__ONE
    if (fglsl__streq(name, "abs")) {
        if (!fglsl__argcount(n, 1, c, name)) return a[0];
        fglsl__val v = a[0];
        v.neg = 0; v.abs = 1; v.lvalue = 0; v.fresh = 0;
        return v;
    }
    if (fglsl__streq(name, "ceil")) {         // -floor(-x)
        if (!fglsl__argcount(n, 1, c, name)) return a[0];
        fglsl__val m = a[0];
        m.neg = !m.neg;
        fglsl__val r = fglsl__op1(c, FGPU_SOP_FLOOR, m, t0);
        r.neg = 1; r.fresh = 0;
        return r;
    }
    if (fglsl__streq(name, "tan")) {
        if (!fglsl__argcount(n, 1, c, name)) return a[0];
        fglsl__val s = fglsl__op1(c, FGPU_SOP_SIN, a[0], t0);
        fglsl__val k = fglsl__op1(c, FGPU_SOP_COS, a[0], t0);
        return fglsl__op2(c, FGPU_SOP_DIV, s, k, t0);
    }
    if (fglsl__streq(name, "exp") || fglsl__streq(name, "log")) {
        if (!fglsl__argcount(n, 1, c, name)) return a[0];
        if (name[0] == 'e') return fglsl__op1(c, FGPU_SOP_EXP2, fglsl__binary(c, '*', a[0], fglsl__scalar(c, 1.44269504f)), t0);
        return fglsl__binary(c, '*', fglsl__op1(c, FGPU_SOP_LOG2, a[0], t0), fglsl__scalar(c, 0.69314718f));
    }
    if (fglsl__streq(name, "radians") || fglsl__streq(name, "degrees")) {
        if (!fglsl__argcount(n, 1, c, name)) return a[0];
        return fglsl__binary(c, '*', a[0], fglsl__scalar(c, name[0] == 'r' ? 0.017453293f : 57.29577951f));
    }
    if (fglsl__streq(name, "atan")) {
        if (n == 1) return fglsl__op2(c, FGPU_SOP_ATAN2, a[0], fglsl__scalar(c, 1.0f), t0);
        if (!fglsl__argcount(n, 2, c, name)) return a[0];
        return fglsl__op2(c, FGPU_SOP_ATAN2, a[0], a[1], fglsl__wide(c, a[0], a[1]));
    }
    if (fglsl__streq(name, "pow") || fglsl__streq(name, "min") || fglsl__streq(name, "max") ||
        fglsl__streq(name, "mod") || fglsl__streq(name, "step")) {
        if (!fglsl__argcount(n, 2, c, name)) return a[0];
        uint32_t op = name[0] == 'p' ? FGPU_SOP_POW : name[1] == 'i' ? FGPU_SOP_MIN :
                      name[1] == 'a' ? FGPU_SOP_MAX : name[0] == 'm' ? FGPU_SOP_MOD : FGPU_SOP_STEP;
        return fglsl__op2(c, op, a[0], a[1], fglsl__wide(c, a[0], a[1]));
    }
    if (fglsl__streq(name, "clamp") || fglsl__streq(name, "mix") || fglsl__streq(name, "smoothstep")) {
        if (!fglsl__argcount(n, 3, c, name)) return a[0];
        int type = fglsl__wide(c, fglsl__mkval(0, 0, fglsl__wide(c, a[0], a[1])), a[2]);
        uint32_t op = name[0] == 'c' ? FGPU_SOP_CLAMP : name[0] == 'm' ? FGPU_SOP_MIX : FGPU_SOP_SSTEP;
        return fglsl__op3(c, op, a[0], a[1], a[2], type);
    }
    if (fglsl__streq(name, "dot")) {
        if (!fglsl__argcount(n, 2, c, name)) return a[0];
        if (a[0].type != a[1].type) { fglsl__error(c, "dot() needs two vectors of the same size", 0); return a[0]; }
        if (t0 == FGLSL_FLOAT) return fglsl__binary(c, '*', a[0], a[1]);
        uint32_t op = t0 == 2 ? FGPU_SOP_DP2 : t0 == 3 ? FGPU_SOP_DP3 : FGPU_SOP_DP4;
        return fglsl__op2(c, op, a[0], a[1], FGLSL_FLOAT);
    }
    if (fglsl__streq(name, "cross")) {
        if (!fglsl__argcount(n, 2, c, name)) return a[0];
        if (a[0].type != 3 || a[1].type != 3) { fglsl__error(c, "cross() needs two vec3", 0); return a[0]; }
        return fglsl__op2(c, FGPU_SOP_CROSS, a[0], a[1], FGLSL_VEC3);
    }
    if (fglsl__streq(name, "length") || fglsl__streq(name, "normalize") || fglsl__streq(name, "distance")) {
        fglsl__val v = a[0];
        if (fglsl__streq(name, "distance")) {
            if (!fglsl__argcount(n, 2, c, name)) return a[0];
            v = fglsl__binary(c, '-', a[0], a[1]);
        } else if (!fglsl__argcount(n, 1, c, name)) {
            return a[0];
        }
        int t = v.type;
        uint32_t dp = t == 1 ? FGPU_SOP_MUL : t == 2 ? FGPU_SOP_DP2 : t == 3 ? FGPU_SOP_DP3 : FGPU_SOP_DP4;
        fglsl__val d = fglsl__op2(c, dp, v, v, FGLSL_FLOAT);
        if (name[0] != 'n') return fglsl__op1(c, FGPU_SOP_SQRT, d, FGLSL_FLOAT);
        fglsl__val r = fglsl__op1(c, FGPU_SOP_RSQ, d, FGLSL_FLOAT);
        return fglsl__op2(c, FGPU_SOP_MUL, v, r, t);
    }
    if (fglsl__streq(name, "reflect")) {       // I - 2 dot(N, I) N
        if (!fglsl__argcount(n, 2, c, name)) return a[0];
        int t = t0;
        uint32_t dp = t == 1 ? FGPU_SOP_MUL : t == 2 ? FGPU_SOP_DP2 : t == 3 ? FGPU_SOP_DP3 : FGPU_SOP_DP4;
        fglsl__val d = fglsl__op2(c, dp, a[1], a[0], FGLSL_FLOAT);
        fglsl__val d2 = fglsl__op2(c, FGPU_SOP_ADD, d, d, FGLSL_FLOAT);
        d2.neg = 1;
        return fglsl__op3(c, FGPU_SOP_MAD, a[1], d2, a[0], t);
    }
    if (fglsl__streq(name, "texture") || fglsl__streq(name, "texture2D")) {
        if (!fglsl__argcount(n, 2, c, name)) return a[0];
        if (a[0].type != FGLSL_SAMPLER || a[1].type < 2 || a[1].type > 4) {
            fglsl__error(c, "texture() takes a sampler2D and a vec2", 0);
            return fglsl__none();
        }
        fglsl__val r = fglsl__newtemp(c, FGLSL_VEC4);
        fglsl__emit(c, FGPU_SOP_TEX, fglsl__dst(r, 4), fglsl__src(a[1]), (uint32_t)a[0].index, 0);
        c->texture_mask |= 1u << a[0].index;
        return r;
    }
    if (fglsl__streq(name, "int")) {          // truncate toward zero
        if (!fglsl__argcount(n, 1, c, name)) return a[0];
        fglsl__val m = a[0];
        m.abs = 1; m.neg = 0;
        fglsl__val f = fglsl__op1(c, FGPU_SOP_FLOOR, m, t0);
        fglsl__val s = fglsl__op1(c, FGPU_SOP_SIGN, a[0], t0);
        return fglsl__op2(c, FGPU_SOP_MUL, f, s, t0);
    }
    if (fglsl__streq(name, "bool")) {
        if (!fglsl__argcount(n, 1, c, name)) return a[0];
        return fglsl__op2(c, FGPU_SOP_SNE, a[0], fglsl__scalar(c, 0.0f), t0);
    }
    *found = 0;
    return fglsl__none();
}

static fglsl__val fglsl__call(fglsl_program_t* c, const char* name) {
    fglsl__val args[8];
    int n = 0;
    fglsl__next(c);     // '('
    if (!fglsl__is(c, ')')) {
        do {
            if (n >= 8) { fglsl__error(c, "too many arguments", 0); return fglsl__none(); }
            args[n++] = fglsl__ternary(c);
        } while (!c->err && fglsl__accept(c, ','));
    }
    fglsl__expect(c, ')');
    if (c->err) return fglsl__none();
    for (int i = 0; i < c->nfuncs; i++)
        if (fglsl__streq(c->funcs[i].name, name)) return fglsl__call_user(c, &c->funcs[i], args, n);
    int type = fglsl__type_of(name);
    if (type > 0 && type != FGLSL_SAMPLER) {
        if (type == FGLSL_FLOAT && n == 1 && !fglsl__streq(name, "float")) {
            int found;
            fglsl__val r = fglsl__builtin(c, name, args, n, &found);   // int(), bool()
            if (found) return r;
        }
        return fglsl__constructor(c, type, args, n);
    }
    int found;
    fglsl__val r = fglsl__builtin(c, name, args, n, &found);
    if (!found) fglsl__error(c, "unknown function", name);
    return r;
}

static fglsl__val fglsl__variable(fglsl_program_t* c, const char* name) {
    fglsl__sym* s = fglsl__find(c, name);
    if (s) return s->v;
    if (fglsl__streq(name, "true")) return fglsl__scalar(c, 1.0f);
    if (fglsl__streq(name, "false")) return fglsl__scalar(c, 0.0f);
    if (!c->fragment && fglsl__streq(name, "gl_Position")) {
        fglsl__val v = fglsl__mkval(FGPU_FILE_OUTPUT, 0, FGLSL_VEC4);
        v.lvalue = 1;
        return v;
    }
    if (c->fragment && fglsl__streq(name, "gl_FragColor")) {
        fglsl__val v = fglsl__mkval(FGPU_FILE_OUTPUT, 0, FGLSL_VEC4);
        v.lvalue = 1;
        return v;
    }
    if (c->fragment && fglsl__streq(name, "gl_FragCoord")) return fglsl__mkval(FGPU_FILE_SPECIAL, 0, FGLSL_VEC4);
    if (c->fragment && fglsl__streq(name, "gl_FrontFacing")) return fglsl__mkval(FGPU_FILE_SPECIAL, 1, FGLSL_FLOAT);
    fglsl__error(c, "unknown identifier", name);
    return fglsl__none();
}

static fglsl__val fglsl__primary(fglsl_program_t* c) {
    if (c->err) return fglsl__none();
    if (fglsl__is(c, FGLSL__TK_NUM)) {
        float x = c->lx.num;
        fglsl__next(c);
        return fglsl__scalar(c, x);
    }
    if (fglsl__accept(c, '(')) {
        fglsl__val v = fglsl__expr(c);
        fglsl__expect(c, ')');
        return v;
    }
    if (fglsl__is(c, FGLSL__TK_ID)) {
        char name[FGLSL_NAME_MAX];
        fglsl__strcpy(name, c->lx.id, FGLSL_NAME_MAX);
        fglsl__next(c);
        if (fglsl__is(c, '(')) return fglsl__call(c, name);
        return fglsl__variable(c, name);
    }
    fglsl__error(c, "expected an expression", 0);
    return fglsl__none();
}

static fglsl__val fglsl__unary(fglsl_program_t* c) {
    if (fglsl__accept(c, '-')) {
        fglsl__val v = fglsl__unary(c);
        if (v.type == FGLSL_MAT4 || v.count) { fglsl__error(c, "cannot negate this value", 0); return v; }
        v.neg = !v.neg;
        v.lvalue = 0;
        v.fresh = 0;
        return v;
    }
    if (fglsl__accept(c, '+')) return fglsl__unary(c);
    if (fglsl__accept(c, '!')) {
        fglsl__val v = fglsl__unary(c);
        if (v.type != FGLSL_FLOAT) { fglsl__error(c, "! works on scalars only", 0); return v; }
        return fglsl__op2(c, FGPU_SOP_SEQ, v, fglsl__scalar(c, 0.0f), FGLSL_FLOAT);
    }
    return fglsl__postfix(c, fglsl__primary(c));
}

static int fglsl__prec(int tok) {
    switch (tok) {
        case FGLSL__TK_OR: return 1;
        case FGLSL__TK_AND: return 2;
        case FGLSL__TK_EQ: case FGLSL__TK_NE: return 3;
        case '<': case '>': case FGLSL__TK_LE: case FGLSL__TK_GE: return 4;
        case '+': case '-': return 5;
        case '*': case '/': return 6;
        default: return 0;
    }
}

static fglsl__val fglsl__binexpr(fglsl_program_t* c, int min_prec) {
    fglsl__val a = fglsl__unary(c);
    for (;;) {
        int tok = c->lx.tok, p = fglsl__prec(tok);
        if (!p || p < min_prec || c->err) return a;
        fglsl__next(c);
        fglsl__val b = fglsl__binexpr(c, p + 1);
        a = fglsl__binary(c, tok, a, b);
    }
}

static fglsl__val fglsl__ternary(fglsl_program_t* c) {
    fglsl__val cond = fglsl__binexpr(c, 1);
    if (!fglsl__accept(c, '?')) return cond;
    fglsl__val a = fglsl__ternary(c);
    fglsl__expect(c, ':');
    fglsl__val b = fglsl__ternary(c);
    if (c->err) return a;
    if (cond.type != FGLSL_FLOAT) { fglsl__error(c, "condition must be a scalar", 0); return a; }
    if (!fglsl__vec_only(c, a) || !fglsl__vec_only(c, b)) return a;
    return fglsl__op3(c, FGPU_SOP_SEL, cond, a, b, fglsl__wide(c, a, b));
}

static fglsl__val fglsl__expr(fglsl_program_t* c) { return fglsl__ternary(c); }

/* ----------------------------------------------------- user functions */

static fglsl__val fglsl__call_user(fglsl_program_t* c, fglsl__func* f, fglsl__val* args, int n) {
    if (n != f->nparams) { fglsl__error(c, "wrong number of arguments to", f->name); return fglsl__none(); }
    if (c->inline_depth >= 12) { fglsl__error(c, "functions nest too deeply (recursion is not allowed):", f->name); return fglsl__none(); }
    for (int i = 0; i < n; i++)
        if (args[i].type != f->ptype[i] && !(args[i].type == FGLSL_FLOAT && f->ptype[i] <= 4)) {
            fglsl__error(c, "argument type mismatch calling", f->name);
            return fglsl__none();
        }
    fglsl__val result = f->ret != FGLSL_VOID ? fglsl__newtemp(c, f->ret) : fglsl__none();
    int keep = c->ntemp;

    // Parameters are local copies (samplers are passed through).
    c->scope++;
    int sym_mark = c->nsyms;
    for (int i = 0; i < n; i++) {
        fglsl__val p;
        if (f->ptype[i] == FGLSL_SAMPLER) {
            p = args[i];
        } else {
            p = fglsl__newtemp(c, f->ptype[i]);
            p.lvalue = 1;
            p.fresh = 0;
            fglsl__val a = args[i];
            a.fresh = 0;
            fglsl__store(c, p, a);
        }
        fglsl__declare(c, f->pname[i], p);
    }

    // Compile the body here, then come back.
    uint32_t list[FGLSL__MAX_PATCH];
    int nlist = 0;
    uint32_t* s_list = c->ret_list; int* s_n = c->ret_n;
    fglsl__val s_val = c->ret_val; int s_type = c->ret_type;
    uint32_t* s_brk = c->brk_list; int* s_brkn = c->brk_n;
    uint32_t* s_cont = c->cont_list; int* s_contn = c->cont_n;
    c->ret_list = list; c->ret_n = &nlist; c->ret_val = result; c->ret_type = f->ret;
    c->brk_list = 0; c->cont_list = 0;
    fglsl__lex saved = c->lx;
    c->lx = f->body;
    c->inline_depth++;
    fglsl__block(c);
    c->inline_depth--;
    int line = c->lx.line;
    c->lx = saved;
    if (c->err && line) { /* error already names the line inside the function */ }
    fglsl__patch(c, list, nlist);
    c->ret_list = s_list; c->ret_n = s_n; c->ret_val = s_val; c->ret_type = s_type;
    c->brk_list = s_brk; c->brk_n = s_brkn; c->cont_list = s_cont; c->cont_n = s_contn;
    c->nsyms = sym_mark;
    c->scope--;
    c->ntemp = keep;
    result.fresh = 0;
    result.lvalue = 0;
    return result;
}

/* --------------------------------------------------------- statements */

static void fglsl__add_jump(fglsl_program_t* c, uint32_t* list, int* n, uint32_t pc) {
    if (!list) return;
    if (*n >= FGLSL__MAX_PATCH) { fglsl__error(c, "too many jumps", 0); return; }
    list[(*n)++] = pc;
}

static void fglsl__declaration(fglsl_program_t* c, int type, int is_const) {
    do {
        if (!fglsl__is(c, FGLSL__TK_ID)) { fglsl__error(c, "expected a variable name", 0); return; }
        char name[FGLSL_NAME_MAX];
        fglsl__strcpy(name, c->lx.id, FGLSL_NAME_MAX);
        fglsl__next(c);
        int count = 0;
        if (fglsl__accept(c, '[')) {
            if (!fglsl__is(c, FGLSL__TK_NUM)) { fglsl__error(c, "array size must be a number", 0); return; }
            count = (int)c->lx.num;
            fglsl__next(c);
            fglsl__expect(c, ']');
            if (count < 1 || count > 16) { fglsl__error(c, "array size out of range", name); return; }
        }
        if (type == FGLSL_SAMPLER || type == FGLSL_VOID) { fglsl__error(c, "invalid local variable type", name); return; }
        if (is_const && !count && fglsl__accept(c, '=')) {
            fglsl__val v = fglsl__expr(c);
            if (fglsl__is_const(v) && (v.type == type || v.type == FGLSL_FLOAT)) {
                if (v.type != type) { v = fglsl__bcast(v); v.type = type; }
                fglsl__declare(c, name, v);
                continue;
            }
            // Not foldable: an ordinary variable.
            fglsl__val var = fglsl__newtemp(c, type);
            var.lvalue = 1; var.fresh = 0;
            fglsl__store(c, var, v);
            var.lvalue = 0;
            fglsl__declare(c, name, var);
            continue;
        }
        int regs = fglsl__regs(type) * (count ? count : 1);
        fglsl__val var = fglsl__mkval(FGPU_FILE_TEMP, fglsl__temp(c, regs), type);
        var.count = count;
        var.lvalue = 1;
        int mark = c->ntemp;
        if (!count && fglsl__accept(c, '=')) {
            fglsl__val v = fglsl__expr(c);
            fglsl__store(c, var, v);
        } else {
            // Start at zero, so results never depend on old register contents.
            fglsl__val zero = fglsl__scalar(c, 0.0f);
            for (int r = 0; r < regs; r++)
                fglsl__emit(c, FGPU_SOP_MOV, FGPU_DST(FGPU_FILE_TEMP, var.index + r, 0xF), fglsl__src(zero), 0, 0);
        }
        c->ntemp = mark;
        if (is_const) var.lvalue = 0;
        fglsl__declare(c, name, var);
    } while (!c->err && fglsl__accept(c, ','));
    fglsl__expect(c, ';');
}

static void fglsl__if(fglsl_program_t* c) {
    fglsl__expect(c, '(');
    int mark = c->ntemp;
    fglsl__val cond = fglsl__expr(c);
    fglsl__expect(c, ')');
    if (c->err) return;
    if (cond.type != FGLSL_FLOAT) { fglsl__error(c, "if needs a scalar condition", 0); return; }
    uint32_t jz = fglsl__emit(c, FGPU_SOP_JZ, 0, fglsl__src(cond), 0, 0);
    c->ntemp = mark;
    fglsl__statement(c);
    if (fglsl__isword(c, "else")) {
        fglsl__next(c);
        uint32_t jmp = fglsl__emit(c, FGPU_SOP_JMP, 0, 0, 0, 0);
        fglsl__patch(c, &jz, 1);
        fglsl__statement(c);
        fglsl__patch(c, &jmp, 1);
    } else {
        fglsl__patch(c, &jz, 1);
    }
}

// for (T i = A; i OP B; i++ / i-- / i += K / i -= K) - unrolled.
static void fglsl__for(fglsl_program_t* c) {
    fglsl__expect(c, '(');
    if (!fglsl__is(c, FGLSL__TK_ID) || fglsl__type_of(c->lx.id) != FGLSL_FLOAT) {
        fglsl__error(c, "for loops must declare an int or float counter", 0);
        return;
    }
    fglsl__next(c);
    char var[FGLSL_NAME_MAX];
    if (!fglsl__is(c, FGLSL__TK_ID)) { fglsl__error(c, "expected a loop counter name", 0); return; }
    fglsl__strcpy(var, c->lx.id, FGLSL_NAME_MAX);
    fglsl__next(c);
    fglsl__expect(c, '=');
    float start = fglsl__const_int(c, fglsl__expr(c), "loop start");
    fglsl__expect(c, ';');
    if (!fglsl__isword(c, var)) { fglsl__error(c, "loop condition must test the counter", var); return; }
    fglsl__next(c);
    int rel = c->lx.tok;
    if (rel != '<' && rel != '>' && rel != FGLSL__TK_LE && rel != FGLSL__TK_GE && rel != FGLSL__TK_NE) {
        fglsl__error(c, "loop condition must be <, <=, >, >= or !=", 0);
        return;
    }
    fglsl__next(c);
    float end = fglsl__const_int(c, fglsl__expr(c), "loop end");
    fglsl__expect(c, ';');
    float step = 1.0f;
    if (fglsl__is(c, FGLSL__TK_INC) || fglsl__is(c, FGLSL__TK_DEC)) {      // ++i / --i
        step = fglsl__is(c, FGLSL__TK_INC) ? 1.0f : -1.0f;
        fglsl__next(c);
        if (!fglsl__isword(c, var)) { fglsl__error(c, "loop step must change the counter", 0); return; }
        fglsl__next(c);
    } else {
        if (!fglsl__isword(c, var)) { fglsl__error(c, "loop step must change the counter", 0); return; }
        fglsl__next(c);
        if (fglsl__accept(c, FGLSL__TK_INC)) step = 1.0f;
        else if (fglsl__accept(c, FGLSL__TK_DEC)) step = -1.0f;
        else if (fglsl__accept(c, FGLSL__TK_ADDEQ)) step = fglsl__const_int(c, fglsl__expr(c), "loop step");
        else if (fglsl__accept(c, FGLSL__TK_SUBEQ)) step = -fglsl__const_int(c, fglsl__expr(c), "loop step");
        else { fglsl__error(c, "write the loop step as i++, i--, i += n or i -= n", 0); return; }
    }
    fglsl__expect(c, ')');
    if (c->err) return;
    if (step == 0.0f) { fglsl__error(c, "loop step is zero", 0); return; }

    fglsl__lex body = c->lx;
    uint32_t brk[FGLSL__MAX_PATCH];
    int nbrk = 0;
    uint32_t* s_brk = c->brk_list; int* s_brkn = c->brk_n;
    uint32_t* s_cont = c->cont_list; int* s_contn = c->cont_n;
    c->brk_list = brk; c->brk_n = &nbrk;
    int iterations = 0;
    fglsl__lex after = body;
    for (float i = start;; i += step) {
        int go = rel == '<' ? i < end : rel == '>' ? i > end : rel == FGLSL__TK_LE ? i <= end :
                 rel == FGLSL__TK_GE ? i >= end : i != end;
        if (!go || c->err) break;
        if (++iterations > 256) { fglsl__error(c, "loop runs more than 256 times", 0); break; }
        uint32_t cont[FGLSL__MAX_PATCH];
        int ncont = 0;
        c->cont_list = cont; c->cont_n = &ncont;
        c->lx = body;
        c->scope++;
        int sym_mark = c->nsyms, temp_mark = c->ntemp;
        fglsl__declare(c, var, fglsl__scalar(c, i));
        fglsl__statement(c);
        after = c->lx;
        c->nsyms = sym_mark;
        c->ntemp = temp_mark;
        c->scope--;
        fglsl__patch(c, cont, ncont);
    }
    // Continue after the body (skip it if the loop never ran).
    c->lx = body;
    if (iterations > 0) c->lx = after;
    else fglsl__skip(c);
    fglsl__patch(c, brk, nbrk);
    c->brk_list = s_brk; c->brk_n = s_brkn;
    c->cont_list = s_cont; c->cont_n = s_contn;
}

static void fglsl__block(fglsl_program_t* c) {
    fglsl__expect(c, '{');
    c->scope++;
    int sym_mark = c->nsyms, temp_mark = c->ntemp;
    while (!c->err && !fglsl__is(c, '}') && !fglsl__is(c, FGLSL__TK_EOF)) fglsl__statement(c);
    fglsl__expect(c, '}');
    c->nsyms = sym_mark;
    c->ntemp = temp_mark;
    c->scope--;
}

static int fglsl__assign_op(int tok) {
    return tok == '=' || tok == FGLSL__TK_ADDEQ || tok == FGLSL__TK_SUBEQ ||
           tok == FGLSL__TK_MULEQ || tok == FGLSL__TK_DIVEQ;
}

static void fglsl__statement(fglsl_program_t* c) {
    if (c->err) return;
    if (fglsl__is(c, '{')) { fglsl__block(c); return; }
    if (fglsl__accept(c, ';')) return;
    if (fglsl__is(c, FGLSL__TK_ID)) {
        const char* w = c->lx.id;
        if (fglsl__streq(w, "if")) { fglsl__next(c); fglsl__if(c); return; }
        if (fglsl__streq(w, "for")) { fglsl__next(c); fglsl__for(c); return; }
        if (fglsl__streq(w, "while") || fglsl__streq(w, "do")) {
            fglsl__error(c, "only for loops with constant bounds are supported", 0);
            return;
        }
        if (fglsl__streq(w, "discard")) {
            fglsl__next(c);
            fglsl__expect(c, ';');
            if (!c->fragment) { fglsl__error(c, "discard is only allowed in the fragment shader", 0); return; }
            fglsl__emit(c, FGPU_SOP_KILL, 0, fglsl__src(fglsl__scalar(c, 1.0f)), 0, 0);
            c->flags |= FGPU_SHADER_FLAG_DISCARD;
            return;
        }
        if (fglsl__streq(w, "break") || fglsl__streq(w, "continue")) {
            int is_break = w[0] == 'b';
            fglsl__next(c);
            fglsl__expect(c, ';');
            uint32_t* list = is_break ? c->brk_list : c->cont_list;
            if (!list) { fglsl__error(c, is_break ? "break outside a loop" : "continue outside a loop", 0); return; }
            uint32_t pc = fglsl__emit(c, FGPU_SOP_JMP, 0, 0, 0, 0);
            fglsl__add_jump(c, list, is_break ? c->brk_n : c->cont_n, pc);
            return;
        }
        if (fglsl__streq(w, "return")) {
            fglsl__next(c);
            int mark = c->ntemp;
            if (!fglsl__is(c, ';')) {
                fglsl__val v = fglsl__expr(c);
                if (c->ret_type == FGLSL_VOID) { fglsl__error(c, "void function returns a value", 0); return; }
                fglsl__val d = c->ret_val;
                d.lvalue = 1;
                d.fresh = 0;
                fglsl__store(c, d, v);
            } else if (c->ret_type != FGLSL_VOID) {
                fglsl__error(c, "missing return value", 0);
                return;
            }
            fglsl__expect(c, ';');
            c->ntemp = mark;
            uint32_t pc = fglsl__emit(c, FGPU_SOP_JMP, 0, 0, 0, 0);
            fglsl__add_jump(c, c->ret_list, c->ret_n, pc);
            return;
        }
        int is_const = 0;
        while (fglsl__isword(c, "const") || fglsl__isword(c, "highp") || fglsl__isword(c, "mediump") ||
               fglsl__isword(c, "lowp")) {
            if (fglsl__isword(c, "const")) is_const = 1;
            fglsl__next(c);
        }
        int type = c->lx.tok == FGLSL__TK_ID ? fglsl__type_of(c->lx.id) : -1;
        if (type >= 0) {
            // A type name followed by '(' is a constructor expression.
            fglsl__lex save = c->lx;
            fglsl__next(c);
            if (!fglsl__is(c, '(')) { fglsl__declaration(c, type, is_const); return; }
            c->lx = save;
        }
    }
    // Expression statement: assignment, ++/--, or a call.
    int mark = c->ntemp;
    int pre = 0;
    if (fglsl__accept(c, FGLSL__TK_INC)) pre = '+';
    else if (fglsl__accept(c, FGLSL__TK_DEC)) pre = '-';
    fglsl__val lhs = fglsl__unary(c);
    if (c->err) return;
    if (pre || fglsl__is(c, FGLSL__TK_INC) || fglsl__is(c, FGLSL__TK_DEC)) {
        int op = pre ? pre : (fglsl__is(c, FGLSL__TK_INC) ? '+' : '-');
        if (!pre) fglsl__next(c);
        fglsl__val r = fglsl__binary(c, op, lhs, fglsl__scalar(c, 1.0f));
        fglsl__store(c, lhs, r);
    } else if (fglsl__assign_op(c->lx.tok)) {
        int tok = c->lx.tok;
        fglsl__next(c);
        fglsl__val rhs = fglsl__expr(c);
        if (tok != '=') {
            int op = tok == FGLSL__TK_ADDEQ ? '+' : tok == FGLSL__TK_SUBEQ ? '-' : tok == FGLSL__TK_MULEQ ? '*' : '/';
            fglsl__val src = lhs;
            src.fresh = 0;
            rhs = fglsl__binary(c, op, src, rhs);
        }
        fglsl__store(c, lhs, rhs);
    }
    fglsl__expect(c, ';');
    c->ntemp = mark;
}

/* ------------------------------------------------------------ globals */

// Default attribute locations by name (FGLVertex order).
static int fglsl__attrib_location(const char* name) {
    char low[FGLSL_NAME_MAX];
    const char* s = name;
    if ((s[0] == 'a' || s[0] == 'v') && s[1] == '_') s += 2;
    else if (s[0] == 'i' && s[1] == 'n' && s[2] == '_') s += 3;
    else if (s[0] == 'a' && s[1] >= 'A' && s[1] <= 'Z') s += 1;
    else if (s[0] == 'i' && s[1] == 'n' && s[2] >= 'A' && s[2] <= 'Z') s += 2;
    int n = 0;
    for (; s[n] && n < FGLSL_NAME_MAX - 1; n++) low[n] = (char)(s[n] >= 'A' && s[n] <= 'Z' ? s[n] + 32 : s[n]);
    low[n] = '\0';
    if (fglsl__streq(low, "position") || fglsl__streq(low, "pos") || fglsl__streq(low, "vertex")) return 0;
    if (fglsl__streq(low, "normal")) return 1;
    if (fglsl__streq(low, "color") || fglsl__streq(low, "colour")) return 2;
    if (fglsl__streq(low, "uv") || fglsl__streq(low, "texcoord") || fglsl__streq(low, "texcoord0")) return 3;
    return -1;
}

static fglsl_uniform_t* fglsl__uniform_named(fglsl_program_t* c, const char* name) {
    for (uint32_t i = 0; i < c->uniform_count; i++)
        if (fglsl__streq(c->uniforms[i].name, name)) return &c->uniforms[i];
    return 0;
}

static void fglsl__global_var(fglsl_program_t* c, int qual, int type, int location) {
    // qual: 'u' uniform, 'i' in/attribute/varying-in, 'o' out/varying-out, 0 none, 'c' const
    do {
        if (!fglsl__is(c, FGLSL__TK_ID)) { fglsl__error(c, "expected a name", 0); return; }
        char name[FGLSL_NAME_MAX];
        fglsl__strcpy(name, c->lx.id, FGLSL_NAME_MAX);
        fglsl__next(c);
        int count = 0;
        if (fglsl__accept(c, '[')) {
            if (!fglsl__is(c, FGLSL__TK_NUM)) { fglsl__error(c, "array size must be a number", 0); return; }
            count = (int)c->lx.num;
            fglsl__next(c);
            fglsl__expect(c, ']');
            if (count < 1 || count > 64) { fglsl__error(c, "array size out of range", name); return; }
        }
        if (qual == 'u') {
            fglsl_uniform_t* u = fglsl__uniform_named(c, name);
            if (u) {
                if (u->type != type || u->array != (uint32_t)count) {
                    fglsl__error(c, "uniform declared differently in the two shaders:", name);
                    return;
                }
            } else {
                if (c->uniform_count >= FGLSL_MAX_UNIFORMS) { fglsl__error(c, "too many uniforms", 0); return; }
                u = &c->uniforms[c->uniform_count++];
                fglsl__strcpy(u->name, name, FGLSL_NAME_MAX);
                u->type = type;
                u->array = (uint32_t)count;
                if (type == FGLSL_SAMPLER) {
                    if (count) { fglsl__error(c, "sampler arrays are not supported", 0); return; }
                    if (c->next_unit >= FGPU_MAX_TEXTURE_UNITS) { fglsl__error(c, "too many samplers", 0); return; }
                    u->reg = c->next_unit++;
                    u->regs = 0;
                } else {
                    u->regs = (uint32_t)(fglsl__regs(type) * (count ? count : 1));
                    u->reg = c->next_uniform_reg;
                    c->next_uniform_reg += u->regs;
                    if (c->next_uniform_reg > FGPU_SHADER_MAX_UNIFORMS) {
                        fglsl__error(c, "too many uniform registers (64 vec4, a mat4 takes 4)", 0);
                        return;
                    }
                }
            }
            fglsl__val v = fglsl__mkval(type == FGLSL_SAMPLER ? FGPU_FILE_NONE : FGPU_FILE_UNIFORM, (int)u->reg, type);
            v.count = count;
            fglsl__declare(c, name, v);
        } else if (qual == 'i' && !c->fragment) {       // vertex attribute
            if (count || type < 1 || type > 4) { fglsl__error(c, "attributes must be float or vec2-4:", name); return; }
            int loc = location >= 0 ? location : fglsl__attrib_location(name);
            if (loc < 0) {
                for (loc = 0; loc < (int)FGPU_MAX_ATTRIBS; loc++)
                    if (!(c->attrib_mask & (1u << loc)) && loc > 3) break;
            }
            if (loc >= (int)FGPU_MAX_ATTRIBS) { fglsl__error(c, "too many attributes", 0); return; }
            if (c->attrib_mask & (1u << loc)) { fglsl__error(c, "two attributes use the same location:", name); return; }
            c->attrib_mask |= 1u << loc;
            fglsl_attrib_t* a = &c->attribs[c->attrib_count++];
            fglsl__strcpy(a->name, name, FGLSL_NAME_MAX);
            a->type = type;
            a->location = (uint32_t)loc;
            fglsl__declare(c, name, fglsl__mkval(FGPU_FILE_INPUT, loc, type));
        } else if (qual == 'o' && c->fragment) {        // fragment output
            if (type != FGLSL_VEC4 || count) { fglsl__error(c, "the fragment output must be a vec4", name); return; }
            fglsl__val v = fglsl__mkval(FGPU_FILE_OUTPUT, 0, FGLSL_VEC4);
            v.lvalue = 1;
            fglsl__declare(c, name, v);
        } else if (qual == 'i' || qual == 'o') {        // varying
            if (count || type < 1 || type > 4) { fglsl__error(c, "varyings must be float or vec2-4:", name); return; }
            int slot = -1;
            for (int i = 0; i < c->nvaryings; i++)
                if (fglsl__streq(c->varyings[i].name, name)) slot = i;
            if (c->fragment) {
                if (slot < 0) { fglsl__error(c, "the vertex shader does not write varying", name); return; }
                if (c->varyings[slot].type != type) { fglsl__error(c, "varying has a different type in the vertex shader:", name); return; }
                fglsl__declare(c, name, fglsl__mkval(FGPU_FILE_INPUT, slot, type));
            } else {
                if (slot >= 0) { fglsl__error(c, "varying declared twice:", name); return; }
                if (c->nvaryings >= (int)FGPU_SHADER_MAX_VARYINGS) { fglsl__error(c, "too many varyings (8 at most)", 0); return; }
                slot = c->nvaryings++;
                fglsl__strcpy(c->varyings[slot].name, name, FGLSL_NAME_MAX);
                c->varyings[slot].type = type;
                fglsl__val v = fglsl__mkval(FGPU_FILE_OUTPUT, 1 + slot, type);
                v.lvalue = 1;
                fglsl__declare(c, name, v);
            }
        } else {
            // Plain global variable or constant: lives in a temp for the whole stage.
            if (type == FGLSL_SAMPLER) { fglsl__error(c, "samplers must be uniforms", name); return; }
            if (qual == 'c' && !count && fglsl__accept(c, '=')) {
                fglsl__val v = fglsl__expr(c);
                if (fglsl__is_const(v) && (v.type == type || v.type == FGLSL_FLOAT)) {
                    if (v.type != type) { v = fglsl__bcast(v); v.type = type; }
                    fglsl__declare(c, name, v);
                    continue;
                }
                fglsl__val var = fglsl__newtemp(c, type);
                var.lvalue = 1; var.fresh = 0;
                fglsl__store(c, var, v);
                var.lvalue = 0;
                fglsl__declare(c, name, var);
                continue;
            }
            int regs = fglsl__regs(type) * (count ? count : 1);
            fglsl__val var = fglsl__mkval(FGPU_FILE_TEMP, fglsl__temp(c, regs), type);
            var.count = count;
            var.lvalue = qual != 'c';
            fglsl__val zero = fglsl__scalar(c, 0.0f);
            for (int r = 0; r < regs; r++)
                fglsl__emit(c, FGPU_SOP_MOV, FGPU_DST(FGPU_FILE_TEMP, var.index + r, 0xF), fglsl__src(zero), 0, 0);
            if (!count && fglsl__accept(c, '=')) {
                int mark = c->ntemp;
                fglsl__val v = fglsl__expr(c);
                fglsl__val w = var;
                w.lvalue = 1;
                fglsl__store(c, w, v);
                c->ntemp = mark;
            }
            fglsl__declare(c, name, var);
        }
    } while (!c->err && fglsl__accept(c, ','));
    fglsl__expect(c, ';');
}

static void fglsl__function(fglsl_program_t* c, int ret, const char* name) {
    fglsl__next(c);     // '('
    fglsl__func f;
    fglsl__strcpy(f.name, name, FGLSL_NAME_MAX);
    f.ret = ret;
    f.nparams = 0;
    if (fglsl__isword(c, "void")) {
        fglsl__next(c);
    } else {
        while (!c->err && !fglsl__is(c, ')')) {
            while (fglsl__isword(c, "in") || fglsl__isword(c, "const") || fglsl__isword(c, "highp") ||
                   fglsl__isword(c, "mediump") || fglsl__isword(c, "lowp")) fglsl__next(c);
            if (fglsl__isword(c, "out") || fglsl__isword(c, "inout")) {
                fglsl__error(c, "out and inout parameters are not supported", 0);
                return;
            }
            int t = c->lx.tok == FGLSL__TK_ID ? fglsl__type_of(c->lx.id) : -1;
            if (t <= 0) { fglsl__error(c, "expected a parameter type", 0); return; }
            fglsl__next(c);
            if (!fglsl__is(c, FGLSL__TK_ID)) { fglsl__error(c, "expected a parameter name", 0); return; }
            if (f.nparams >= 8) { fglsl__error(c, "too many parameters", 0); return; }
            fglsl__strcpy(f.pname[f.nparams], c->lx.id, FGLSL_NAME_MAX);
            f.ptype[f.nparams++] = t;
            fglsl__next(c);
            if (!fglsl__accept(c, ',')) break;
        }
    }
    fglsl__expect(c, ')');
    if (fglsl__accept(c, ';')) return;          // prototype
    if (!fglsl__is(c, '{')) { fglsl__error(c, "expected a function body", 0); return; }
    f.body = c->lx;
    fglsl__skip(c);
    if (fglsl__streq(name, "main")) {
        if (ret != FGLSL_VOID || f.nparams) { fglsl__error(c, "main must be void main()", 0); return; }
        c->main_body = f.body;
        c->has_main = 1;
        return;
    }
    if (c->nfuncs >= FGLSL__MAX_FUNCS) { fglsl__error(c, "too many functions", 0); return; }
    c->funcs[c->nfuncs++] = f;
}

static void fglsl__globals(fglsl_program_t* c) {
    while (!c->err && !fglsl__is(c, FGLSL__TK_EOF)) {
        if (fglsl__accept(c, ';')) continue;
        if (fglsl__isword(c, "precision")) { fglsl__skip(c); continue; }
        if (fglsl__isword(c, "struct")) { fglsl__error(c, "structs are not supported", 0); return; }
        int qual = 0, location = -1;
        for (;;) {
            if (fglsl__isword(c, "layout")) {
                fglsl__next(c);
                fglsl__expect(c, '(');
                if (fglsl__isword(c, "location")) {
                    fglsl__next(c);
                    fglsl__expect(c, '=');
                    if (fglsl__is(c, FGLSL__TK_NUM)) { location = (int)c->lx.num; fglsl__next(c); }
                    if (location < 0 || location >= (int)FGPU_MAX_ATTRIBS) { fglsl__error(c, "location out of range", 0); return; }
                }
                while (!c->err && !fglsl__is(c, ')') && !fglsl__is(c, FGLSL__TK_EOF)) fglsl__next(c);
                fglsl__expect(c, ')');
                continue;
            }
            if (fglsl__isword(c, "uniform")) qual = 'u';
            else if (fglsl__isword(c, "attribute")) qual = 'i';
            else if (fglsl__isword(c, "varying")) qual = c->fragment ? 'i' : 'o';
            else if (fglsl__isword(c, "in")) qual = 'i';
            else if (fglsl__isword(c, "out")) qual = 'o';
            else if (fglsl__isword(c, "const")) qual = 'c';
            else if (!(fglsl__isword(c, "highp") || fglsl__isword(c, "mediump") || fglsl__isword(c, "lowp") ||
                       fglsl__isword(c, "flat") || fglsl__isword(c, "smooth") || fglsl__isword(c, "centroid") ||
                       fglsl__isword(c, "invariant"))) break;
            fglsl__next(c);
        }
        if (c->err) return;
        int type = c->lx.tok == FGLSL__TK_ID ? fglsl__type_of(c->lx.id) : -1;
        if (type < 0) {
            if (c->lx.tok == FGLSL__TK_ID && (fglsl__streq(c->lx.id, "mat3") || fglsl__streq(c->lx.id, "mat2")))
                fglsl__error(c, "only mat4 matrices are supported:", c->lx.id);
            else
                fglsl__error(c, "expected a declaration", c->lx.tok == FGLSL__TK_ID ? c->lx.id : 0);
            return;
        }
        fglsl__next(c);
        if (!fglsl__is(c, FGLSL__TK_ID)) { fglsl__error(c, "expected a name", 0); return; }
        // Function or variable?
        fglsl__lex save = c->lx;
        char name[FGLSL_NAME_MAX];
        fglsl__strcpy(name, c->lx.id, FGLSL_NAME_MAX);
        fglsl__next(c);
        if (fglsl__is(c, '(')) {
            if (qual) { fglsl__error(c, "qualifier on a function", name); return; }
            fglsl__function(c, type, name);
            continue;
        }
        c->lx = save;
        fglsl__global_var(c, qual, type, location);
    }
}

static int fglsl__stage(fglsl_program_t* c, const char* src, int fragment, uint32_t* out_offset,
                        uint32_t* out_count, uint32_t* used) {
    c->fragment = fragment;
    c->lx.src = src ? src : "";
    c->lx.pos = 0;
    c->lx.line = 1;
    c->count = 0;
    c->label_pc = (uint32_t)-1;
    c->nsyms = 0;
    c->scope = 0;
    c->ntemp = 0;
    c->nfuncs = 0;
    c->has_main = 0;
    c->ret_list = 0; c->ret_n = 0; c->ret_type = FGLSL_VOID; c->ret_val = fglsl__none();
    c->brk_list = 0; c->cont_list = 0;
    c->inline_depth = 0;
    fglsl__next(c);
    fglsl__globals(c);
    if (c->err) return -1;
    if (!c->has_main) { fglsl__error(c, "no void main() function", 0); return -1; }
    uint32_t rets[FGLSL__MAX_PATCH];
    int nrets = 0;
    c->ret_list = rets; c->ret_n = &nrets;
    c->lx = c->main_body;
    fglsl__block(c);
    if (c->err) return -1;
    fglsl__patch(c, rets, nrets);
    if (c->count == 0) fglsl__emit(c, FGPU_SOP_NOP, 0, 0, 0, 0);
    if (c->err) return -1;
    uint32_t words = c->count * FGPU_SHADER_INSTRUCTION_WORDS;
    if (*used + words > FGLSL_MAX_BLOB_WORDS) { fglsl__error(c, "program too large", 0); return -1; }
    *out_offset = *used;
    *out_count = c->count;
    for (uint32_t i = 0; i < words; i++) c->words[*used + i] = c->code[i];
    *used += words;
    return 0;
}

/* Compiles a vertex and a fragment shader into one program.
 * Returns 0 on success (c->words / c->size hold the blob), or -1 with the
 * reason in c->error. */
FGLSL__API int fglsl_compile(fglsl_program_t* c, const char* vertex_src, const char* fragment_src) {
    c->size = 0;
    c->error[0] = '\0';
    c->err = 0;
    c->uniform_count = 0;
    c->attrib_count = 0;
    c->pool_count = 0;
    c->nvaryings = 0;
    c->next_uniform_reg = 0;
    c->next_unit = 0;
    c->attrib_mask = 0;
    c->texture_mask = 0;
    c->flags = 0;
    c->max_temp = 0;
    fgpu_shader_header_t h;
    uint32_t used = sizeof(fgpu_shader_header_t) / 4;
    for (uint32_t i = 0; i < used; i++) c->words[i] = 0;
    if (fglsl__stage(c, vertex_src, 0, &h.vs_offset, &h.vs_count, &used) != 0) return -1;
    if (fglsl__stage(c, fragment_src, 1, &h.fs_offset, &h.fs_count, &used) != 0) return -1;
    h.const_offset = used;
    h.const_count = c->pool_count;
    for (uint32_t r = 0; r < c->pool_count; r++) {
        for (int k = 0; k < 4; k++) {
            union { float f; uint32_t u; } v;
            v.f = c->pool[r][k];
            c->words[used++] = v.u;
        }
    }
    h.magic = FGPU_SHADER_MAGIC;
    h.version = FGPU_SHADER_VERSION;
    h.size = used * 4;
    h.flags = c->flags;
    h.attrib_mask = c->attrib_mask;
    h.varying_count = (uint32_t)c->nvaryings;
    h.uniform_count = c->next_uniform_reg;
    h.texture_mask = c->texture_mask;
    h.temp_count = (uint32_t)c->max_temp;
    h.reserved = 0;
    const uint32_t* hw = (const uint32_t*)(const void*)&h;
    for (uint32_t i = 0; i < sizeof(h) / 4; i++) c->words[i] = hw[i];
    c->size = used * 4;
    return 0;
}

/* Index of a uniform in c->uniforms, or -1. */
FGLSL__API int fglsl_find_uniform(const fglsl_program_t* c, const char* name) {
    for (uint32_t i = 0; i < c->uniform_count; i++)
        if (fglsl__streq(c->uniforms[i].name, name)) return (int)i;
    return -1;
}

#endif /* FGLSL_H */
