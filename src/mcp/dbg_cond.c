/*
 * dbg_cond.c — 条件付きブレークポイントの条件式パーサ・評価器の実装。
 */
#include "mcp/dbg_cond.h"
#include "mc68000/cpu.h"
#include "mc68000/debug.h"
#include <string.h>
#include <stdlib.h>

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static int parse_op(const char **p, dbg_cond_op_t *op) {
    const char *s = *p;
    if (s[0] == '=' && s[1] == '=') {
        *op = DBG_COND_EQ;
        *p = s + 2;
        return 0;
    }
    if (s[0] == '!' && s[1] == '=') {
        *op = DBG_COND_NE;
        *p = s + 2;
        return 0;
    }
    if (s[0] == '<' && s[1] == '=') {
        *op = DBG_COND_LE;
        *p = s + 2;
        return 0;
    }
    if (s[0] == '>' && s[1] == '=') {
        *op = DBG_COND_GE;
        *p = s + 2;
        return 0;
    }
    if (s[0] == '<') {
        *op = DBG_COND_LT;
        *p = s + 1;
        return 0;
    }
    if (s[0] == '>') {
        *op = DBG_COND_GT;
        *p = s + 1;
        return 0;
    }
    return -1;
}

static int parse_hex(const char **p, uint32_t *out) {
    const char *s = *p;
    char *end;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s)
        return -1;
    *out = (uint32_t)v;
    *p = end;
    return 0;
}

/* レジスタ名テーブル。D0-D7/A0-A7(2文字)・PC/SR(2文字)・USP/SSP(3文字)は
 * 互いに他の名前の接頭辞にならないため、走査順は結果に影響しない。 */
static const char *const kRegNames[] = {"D0", "D1", "D2", "D3", "D4",  "D5", "D6",
                                        "D7", "A0", "A1", "A2", "A3",  "A4", "A5",
                                        "A6", "A7", "PC", "SR", "USP", "SSP"};
#define NUM_REG_NAMES (int)(sizeof(kRegNames) / sizeof(kRegNames[0]))

static int parse_reg_name(const char **p, char *out /* [4] */) {
    const char *s = *p;
    for (int i = 0; i < NUM_REG_NAMES; i++) {
        size_t len = strlen(kRegNames[i]);
        if (strncmp(s, kRegNames[i], len) == 0) {
            memcpy(out, kRegNames[i], len);
            out[len] = '\0';
            *p = s + len;
            return 0;
        }
    }
    return -1;
}

static int parse_term(const char **p, dbg_cond_term_t *t) {
    const char *s = skip_ws(*p);
    memset(t, 0, sizeof(*t));

    if (*s == '#') {
        s++;
        char *end;
        long n = strtol(s, &end, 10); /* パスカウントは10進（<hex>とは別記法） */
        if (end == s || n < 0)
            return -1;
        t->kind = DBG_COND_TERM_PASSCOUNT;
        t->pass_n = (int)n;
        t->pass_remaining = (int)n;
        *p = end;
        return 0;
    }

    if (*s == '[') {
        s++;
        uint32_t addr;
        if (parse_hex(&s, &addr) != 0)
            return -1;
        s = skip_ws(s);
        if (*s != ']')
            return -1;
        s++;
        int size = 1; /* サフィックス省略時はバイト */
        if (*s == '.') {
            s++;
            if (*s == 'b' || *s == 'B')
                size = 1;
            else if (*s == 'w' || *s == 'W')
                size = 2;
            else if (*s == 'l' || *s == 'L')
                size = 4;
            else
                return -1;
            s++;
        }
        s = skip_ws(s);
        dbg_cond_op_t op;
        if (parse_op(&s, &op) != 0)
            return -1;
        s = skip_ws(s);
        uint32_t rhs;
        if (parse_hex(&s, &rhs) != 0)
            return -1;
        t->kind = DBG_COND_TERM_MEM;
        t->mem_addr = addr;
        t->mem_size = size;
        t->op = op;
        t->rhs = rhs;
        *p = s;
        return 0;
    }

    char regname[4];
    if (parse_reg_name(&s, regname) != 0)
        return -1;
    s = skip_ws(s);
    dbg_cond_op_t op;
    if (parse_op(&s, &op) != 0)
        return -1;
    s = skip_ws(s);
    uint32_t rhs;
    if (parse_hex(&s, &rhs) != 0)
        return -1;
    t->kind = DBG_COND_TERM_REG;
    memcpy(t->reg_name, regname, sizeof(t->reg_name));
    t->op = op;
    t->rhs = rhs;
    *p = s;
    return 0;
}

int dbg_cond_parse(const char *text, dbg_cond_t *out) {
    dbg_cond_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (!text || !*text) {
        *out = tmp;
        return 0;
    }
    const char *p = text;
    for (;;) {
        p = skip_ws(p);
        if (tmp.term_count >= DBG_COND_MAX_TERMS)
            return -1;
        if (parse_term(&p, &tmp.terms[tmp.term_count]) != 0)
            return -1;
        tmp.term_count++;
        p = skip_ws(p);
        if (*p == '\0')
            break;
        if (p[0] != '&' || p[1] != '&')
            return -1;
        p += 2;
    }
    *out = tmp;
    return 0;
}

static uint32_t reg_value(const mc68k_regs_t *r, const char *name) {
    if (name[0] == 'D' && name[1] >= '0' && name[1] <= '7' && name[2] == '\0')
        return r->d[name[1] - '0'];
    if (name[0] == 'A' && name[1] >= '0' && name[1] <= '7' && name[2] == '\0')
        return r->a[name[1] - '0'];
    if (strcmp(name, "PC") == 0)
        return r->pc;
    if (strcmp(name, "SR") == 0)
        return r->sr;
    if (strcmp(name, "USP") == 0)
        return r->usp;
    if (strcmp(name, "SSP") == 0)
        return r->ssp;
    return 0;
}

static int compare_u32(uint32_t lhs, dbg_cond_op_t op, uint32_t rhs) {
    switch (op) {
    case DBG_COND_EQ:
        return lhs == rhs;
    case DBG_COND_NE:
        return lhs != rhs;
    case DBG_COND_LT:
        return lhs < rhs;
    case DBG_COND_GT:
        return lhs > rhs;
    case DBG_COND_LE:
        return lhs <= rhs;
    case DBG_COND_GE:
        return lhs >= rhs;
    default:
        return 0;
    }
}

int dbg_cond_eval(dbg_cond_t *cond, mc68k_state_t *cpu) {
    if (cond->term_count == 0)
        return 1; /* 無条件（term省略時）は常にヒット */
    mc68k_regs_t regs;
    mc68k_get_regs(cpu, &regs);
    int result = 1;
    for (int i = 0; i < cond->term_count; i++) {
        dbg_cond_term_t *t = &cond->terms[i];
        int term_ok;
        if (t->kind == DBG_COND_TERM_REG) {
            term_ok = compare_u32(reg_value(&regs, t->reg_name), t->op, t->rhs);
        } else if (t->kind == DBG_COND_TERM_MEM) {
            uint8_t buf[4] = {0};
            if (mc68k_debug_read_bytes(cpu, t->mem_addr, buf, (uint32_t)t->mem_size) != 0) {
                term_ok = 0;
            } else {
                uint32_t v = 0;
                for (int b = 0; b < t->mem_size; b++)
                    v = (v << 8) | buf[b];
                term_ok = compare_u32(v, t->op, t->rhs);
            }
        } else {
            /* PASSCOUNT: 他項の真偽に関わらず呼ばれるたびにデクリメントする
             * （「最初のn回は無視」は"このBPアドレスに到達した回数"を数える
             * 意図のため、REG/MEM項と独立に進める）。残数が尽きたら真。 */
            if (t->pass_remaining > 0) {
                t->pass_remaining--;
                term_ok = 0;
            } else {
                term_ok = 1;
            }
        }
        if (!term_ok)
            result = 0;
    }
    return result;
}
