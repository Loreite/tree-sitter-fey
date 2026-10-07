// scanner.c
#include "html_names.h"
#include "tree_sitter/parser.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wctype.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define VEC_RESIZE(vec, _cap)                                                  \
  {                                                                            \
    (vec)->data = realloc((vec)->data, (_cap) * sizeof((vec)->data[0]));       \
    assert((vec)->data != NULL);                                               \
    (vec)->cap = (_cap);                                                       \
  }

#define VEC_PUSH(vec, el)                                                      \
  {                                                                            \
    if ((vec)->cap == (vec)->len) {                                            \
      VEC_RESIZE((vec), MAX(16, (vec)->len * 2));                              \
    }                                                                          \
    (vec)->data[(vec)->len++] = (el);                                          \
  }

#define VEC_POP(vec) (vec)->len--;

#define VEC_BACK(vec) ((vec)->data[(vec)->len - 1])

#define VEC_FREE(vec)                                                          \
  {                                                                            \
    if ((vec)->data != NULL)                                                   \
      free((vec)->data);                                                       \
  }

#define VEC_CLEAR(vec)                                                         \
  {                                                                            \
    (vec)->len = 0;                                                            \
  }

#define SER_MAX 1024
#define PUT(v)                                                                 \
  do {                                                                         \
    if (i < SER_MAX)                                                           \
      buffer[i++] = (char)(v);                                                 \
  } while (0)

// Must match `externals` in grammar.js exactly, in the same order.
enum TokenType {
  STANDALONE_TAG_START,
  BLOCK_TAG_START,
  BLOCK_TAG_END,
  LIST_START,
  LIST_END,
  LISTITEM_END,
  BULLET,
  FENCE,
  SIGNATURE,
  SECTION_END,
  ENDOFFILE,
  TAG_NL,
  PAIR_OPEN_START,
  PAIR_BLOCK_START,
  PAIR_CLOSE_START,
  PAIR_STRAY_CLOSE,
  TAG_VALUE_NL,
  PAIR_IMPLICIT_CLOSE,
  LINE_TAG_EOL,
  LINE_TAG_START,
  SCOPE_TAG_START,
  SCOPE_LINE_TAG_START,
  HEAD_END,
  HEAD_END_BARE,
};

// What ends a head, by the form of the tag (the closing bracket is the one
// that matches the opening one):
//   HEAD_SIGIL    a sigil and the bracket     `#}`    scope tag, pair opener
//   HEAD_BRACKET  the bracket                 `]`     line tag
//   HEAD_BLOCK    the bracket and any sign    `]#`    block tag
enum { HEAD_NONE, HEAD_SIGIL, HEAD_BRACKET, HEAD_BLOCK };

typedef enum {
  NOTABULLET,
  ISABULLET,
} Bullet;

typedef struct {
  uint32_t len;
  uint32_t cap;
  int16_t *data;
} stack;

// ---------------------------------------------------------------------------
// Pair-tag name stack
//
// Each open pair tag `[ name #]` is one entry. Names are stored like
// tree-sitter-html does it: known HTML element names as an index into the
// compiled-in HTML_NAMES table, everything else as a 24-bit hash.
//
// flag byte
//   bit 0      0 = symbol table, 1 = 24-bit hash
//   bits 1-2   bracket type   (0 '[', 1 '{', 2 '(', 3 '<')
//   bits 3-7   tag-token index into PAIR_TOKENS (0..18)
// payload
//   table: 1 byte  (index into HTML_NAMES)
//   hash:  3 bytes (big-endian 24-bit FNV-1a fold)
//
// floors (the portal)
//   indent_floor / tag_floor are the lengths of indent_length_stack and
//   tag_indent_length_stack when the pair opened. Inside the body only the
//   entries above the floor count, so the body starts from a fresh
//   indentation root no matter which list item or block tag holds the opener.
//
// PAIR_FORGOTTEN marks an entry whose details did not fit in the serialized
// state (see serialize()); it matches any closer. A real flag is at most
// (18 << 3) | (3 << 1) | 1 = 0x97, so 0xFF can never collide.
// ---------------------------------------------------------------------------

#define PAIR_FLAG_HASH 0x01
#define PAIR_FORGOTTEN 0xFF
#define PAIR_MAX_DEPTH 255
#define PAIR_NAME_BUF 32       // first 32 folded chars kept for table lookup
#define PAIR_LOOKAHEAD_CAP 512 // max chars scanned while classifying an opener

// Same order as `tagTokens` in grammar.js.
static const char PAIR_TOKENS[] = "@#$&%!?\\/-+*=~^.,:;";

typedef struct {
  uint8_t flag;
  uint8_t table_idx;
  uint32_t hash24;
  uint8_t indent_floor;
  uint8_t tag_floor;
} PairEntry;

typedef struct {
  uint32_t len;
  uint32_t cap;
  PairEntry *data;
} pair_stack;

typedef struct {
  stack *indent_length_stack;
  stack *bullet_stack;
  stack *section_stack;

  stack *tag_indent_length_stack;

  stack *fence_indent_stack;
  // Where the opener starts. A fence opened right after a bullet may be closed
  // at any indent from that of the bullet's line to this column; on a line of
  // its own the two are the same.
  stack *fence_col_stack;
  stack *fence_width_stack;
  stack *fence_char_stack;

  pair_stack *pair_stack;

  int16_t block_tag_col;
  bool is_at_section_start;
  // Set when a pair was implicitly closed at the start of a heading line:
  // the text run that held the pair still needs its end of line, so the
  // next scan owes it a zero-width ENDOFFILE (the grammar's _eol).
  bool eol_owed;
  int16_t base_indent;
  // The head being read, and what ends it (HEAD_NONE when none): the scanner
  // recognises a tag before its grammar reads it, and remembers its opening
  // bracket (and sigil) so that the end of the head is exactly the closer that
  // belongs to it. Any other sign or bracket is a word of the head. Set when
  // the tag is recognised, cleared when its end is scanned.
  int16_t head_form;
  int16_t head_sigil;
  int16_t head_close;
} Scanner;

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }
static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

// ---- indentation roots -------------------------------------------------
//
// indent_length_stack has a sentinel at index 0, so outside any pair the
// floor is 1; tag_indent_length_stack has none, so its floor is 0.

static uint32_t indent_floor(Scanner *s) {
  pair_stack *ps = s->pair_stack;
  uint32_t f = ps->len ? ps->data[ps->len - 1].indent_floor : 1;
  return f < 1 ? 1 : f;
}

static uint32_t tag_floor(Scanner *s) {
  pair_stack *ps = s->pair_stack;
  return ps->len ? ps->data[ps->len - 1].tag_floor : 0;
}

// Is a list open in the current indentation root?
static bool in_list(Scanner *s) {
  return s->indent_length_stack->len > indent_floor(s);
}

// Is a block tag open in the current indentation root?
static bool in_block_tag(Scanner *s) {
  return s->tag_indent_length_stack->len > tag_floor(s);
}

// Innermost list indent in the current root, or -1 (the root itself).
static int16_t indent_top(Scanner *s) {
  return in_list(s) ? VEC_BACK(s->indent_length_stack) : -1;
}

static int16_t bullet_top(Scanner *s) {
  return in_list(s) ? VEC_BACK(s->bullet_stack) : NOTABULLET;
}

static int16_t tag_top(Scanner *s) {
  return in_block_tag(s) ? VEC_BACK(s->tag_indent_length_stack) : -1;
}

// ---- pair helpers ----------------------------------------------------------

static int bracket_index(int32_t c) {
  switch (c) {
  case '[':
  case ']':
    return 0;
  case '{':
  case '}':
    return 1;
  case '(':
  case ')':
    return 2;
  case '<':
  case '>':
    return 3;
  default:
    return -1;
  }
}

static int token_index(int32_t c) {
  if (c <= 0 || c > 127)
    return -1;
  const char *p = strchr(PAIR_TOKENS, (int)c);
  return p ? (int)(p - PAIR_TOKENS) : -1;
}

static bool is_name_start(int32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_name_char(int32_t c) {
  return is_name_start(c) || (c >= '0' && c <= '9') || c == '-';
}

static inline uint8_t ascii_fold(int32_t c) {
  return (uint8_t)((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
}

// Name accumulator: streams the FNV-1a hash over the whole folded name and
// keeps the first PAIR_NAME_BUF - 1 folded chars for the table lookup.
typedef struct {
  char buf[PAIR_NAME_BUF];
  size_t len;
  uint32_t fnv;
} NameAcc;

static void name_init(NameAcc *n) {
  n->len = 0;
  n->buf[0] = '\0';
  n->fnv = 2166136261u;
}

static void name_push(NameAcc *n, int32_t c) {
  uint8_t f = ascii_fold(c);
  if (n->len < PAIR_NAME_BUF - 1)
    n->buf[n->len] = (char)f;
  n->len++;
  n->fnv ^= f;
  n->fnv *= 16777619u;
}

static int html_name_lookup(const char *name) {
  int lo = 0, hi = HTML_NAMES_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int cmp = strcmp(name, HTML_NAMES[mid]);
    if (cmp == 0)
      return mid;
    if (cmp < 0)
      hi = mid - 1;
    else
      lo = mid + 1;
  }
  return -1;
}

// Table first, so a known name is never stored as a hash: a table entry and
// a hash entry can never compare equal.
static void classify(const NameAcc *n, bool *is_hash, uint8_t *idx,
                     uint32_t *h24) {
  if (n->len < PAIR_NAME_BUF) {
    char tmp[PAIR_NAME_BUF];
    memcpy(tmp, n->buf, n->len);
    tmp[n->len] = '\0';
    int found = html_name_lookup(tmp);
    if (found >= 0) {
      *is_hash = false;
      *idx = (uint8_t)found;
      *h24 = 0;
      return;
    }
  }
  *is_hash = true;
  *idx = 0;
  *h24 = ((n->fnv >> 24) ^ n->fnv) & 0xFFFFFF;
}

static PairEntry make_entry(const NameAcc *n, int bracket, int token) {
  bool is_hash;
  uint8_t idx;
  uint32_t h24;
  classify(n, &is_hash, &idx, &h24);
  PairEntry e;
  e.flag = (uint8_t)(((token & 0x1F) << 3) | ((bracket & 0x3) << 1) |
                     (is_hash ? PAIR_FLAG_HASH : 0));
  e.table_idx = idx;
  e.hash24 = h24;
  e.indent_floor = 0;
  e.tag_floor = 0;
  return e;
}

static bool entry_matches(PairEntry top, PairEntry closer) {
  if (top.flag == PAIR_FORGOTTEN)
    return true;
  if (top.flag != closer.flag)
    return false;
  return (top.flag & PAIR_FLAG_HASH) ? top.hash24 == closer.hash24
                                     : top.table_idx == closer.table_idx;
}

// ---------------------------------------------------------------------------
// Serialized layout (must match deserialize exactly):
//   [base_indent hi][base_indent lo][eol_owed][head form][head sigil][head close]
//   [fence_len (0|1)] [fence indent][fence col][fence width][fence char]   (if fence_len)
//   [tag_count]    [tag indents ...]        (all entries; no sentinel)
//   [pair_depth hi][pair_depth lo][pair_stored]
//                  [entries ...]            (innermost `pair_stored` entries,
//                                            oldest -> newest; each entry is
//                                            flag + 1 byte (table) or
//                                            flag + 3 bytes (hash), then
//                                            indent_floor + tag_floor)
//   [indent_count] [indents ...][bullets ...]  (entries after the sentinel)
//   [sections ...]                          (entries after the sentinel; rest)
//
// Entries below the stored slice are restored as PAIR_FORGOTTEN.
// ---------------------------------------------------------------------------
static unsigned serialize(Scanner *scanner, char *buffer) {
  size_t i = 0;

  PUT((scanner->base_indent >> 8) & 0xFF);
  PUT(scanner->base_indent & 0xFF);
  PUT(scanner->eol_owed ? 1 : 0);
  PUT(scanner->head_form);
  PUT(scanner->head_sigil);
  PUT(scanner->head_close);

  // fences don't nest: at most one entry is ever live
  if (scanner->fence_indent_stack->len > 0) {
    PUT(1);
    PUT(VEC_BACK(scanner->fence_indent_stack));
    PUT(VEC_BACK(scanner->fence_col_stack));
    PUT(VEC_BACK(scanner->fence_width_stack));
    PUT(VEC_BACK(scanner->fence_char_stack));
  } else {
    PUT(0);
  }

  // tag_indent_length_stack has NO sentinel: serialize every entry
  uint32_t tag_count = scanner->tag_indent_length_stack->len;
  if (tag_count > 128)
    tag_count = 128;
  PUT(tag_count);
  for (uint32_t j = 0; j < tag_count; ++j) {
    int16_t v = scanner->tag_indent_length_stack->data[j];
    PUT(v > 255 ? 255 : v);
  }

  // indent/bullet stacks have a sentinel at index 0: skip it
  uint32_t indent_count = scanner->indent_length_stack->len
                              ? scanner->indent_length_stack->len - 1
                              : 0;
  if (indent_count > 128)
    indent_count = 128;
  uint32_t section_count =
      scanner->section_stack->len ? scanner->section_stack->len - 1 : 0;

  // ---- pair block: keep as many innermost entries as fit -------------------
  pair_stack *ps = scanner->pair_stack;
  size_t tail = 1 + 2 * (size_t)indent_count + section_count;
  long budget = (long)SER_MAX - (long)i - (long)tail - 3;
  uint32_t stored = 0;
  long used = 0;
  while (stored < ps->len && stored < 255) {
    PairEntry e = ps->data[ps->len - 1 - stored];
    long need = (e.flag & PAIR_FLAG_HASH) ? 6 : 4;
    if (e.flag == PAIR_FORGOTTEN || used + need > budget)
      break; // a forgotten entry stays forgotten, and so does all below it
    used += need;
    stored++;
  }
  PUT((ps->len >> 8) & 0xFF);
  PUT(ps->len & 0xFF);
  PUT(stored);
  for (uint32_t j = ps->len - stored; j < ps->len; ++j) {
    PairEntry e = ps->data[j];
    PUT(e.flag);
    if (e.flag & PAIR_FLAG_HASH) {
      PUT((e.hash24 >> 16) & 0xFF);
      PUT((e.hash24 >> 8) & 0xFF);
      PUT(e.hash24 & 0xFF);
    } else {
      PUT(e.table_idx);
    }
    PUT(e.indent_floor);
    PUT(e.tag_floor);
  }

  PUT(indent_count);
  for (uint32_t j = 1; j <= indent_count; ++j) {
    int16_t v = scanner->indent_length_stack->data[j];
    PUT(v > 255 ? 255 : v);
  }
  for (uint32_t j = 1; j <= indent_count; ++j) {
    PUT(scanner->bullet_stack->data[j]);
  }

  for (uint32_t j = 1; j < scanner->section_stack->len && i < SER_MAX; ++j) {
    PUT(scanner->section_stack->data[j]);
  }

  return (unsigned)i;
}

#define GET() ((i < length) ? (uint8_t)buffer[i++] : (i++, 0))
static void deserialize(Scanner *scanner, const char *buffer, unsigned length) {
  VEC_CLEAR(scanner->section_stack);
  VEC_PUSH(scanner->section_stack, 0);
  VEC_CLEAR(scanner->indent_length_stack);
  VEC_PUSH(scanner->indent_length_stack, -1);
  VEC_CLEAR(scanner->bullet_stack);
  VEC_PUSH(scanner->bullet_stack, NOTABULLET);

  VEC_CLEAR(scanner->fence_indent_stack);
  VEC_CLEAR(scanner->fence_col_stack);
  VEC_CLEAR(scanner->fence_width_stack);
  VEC_CLEAR(scanner->fence_char_stack);
  VEC_CLEAR(scanner->tag_indent_length_stack);
  VEC_CLEAR(scanner->pair_stack);

  scanner->base_indent = -1;
  scanner->eol_owed = false;
  scanner->head_form = HEAD_NONE;
  scanner->head_sigil = 0;
  scanner->head_close = 0;

  if (length < 6)
    return;

  size_t i = 0;

  uint8_t base_hi = GET(); // separate statements: operand order is unspecified
  uint8_t base_lo = GET();
  scanner->base_indent = (int16_t)((base_hi << 8) | base_lo);
  scanner->eol_owed = GET() != 0;
  scanner->head_form = GET();
  scanner->head_sigil = GET();
  scanner->head_close = GET();

  uint8_t fence_len = GET();
  if (fence_len > 0) {
    VEC_PUSH(scanner->fence_indent_stack, GET());
    VEC_PUSH(scanner->fence_col_stack, GET());
    VEC_PUSH(scanner->fence_width_stack, GET());
    VEC_PUSH(scanner->fence_char_stack, GET());
  }

  uint8_t tag_count = GET();
  for (uint8_t j = 0; j < tag_count && i <= length; j++) {
    VEC_PUSH(scanner->tag_indent_length_stack, GET());
  }

  uint8_t depth_hi = GET();
  uint8_t depth_lo = GET();
  uint32_t pair_depth = ((uint32_t)depth_hi << 8) | depth_lo;
  uint32_t pair_stored = GET();
  if (pair_stored > pair_depth)
    pair_stored = pair_depth; // corrupt input: never underflow
  // Forgotten entries lost their floors too; 0 means "no portal", which
  // only matters if one of them becomes the innermost entry again.
  PairEntry forgotten = {PAIR_FORGOTTEN, 0, 0, 0, 0};
  for (uint32_t j = 0; j < pair_depth - pair_stored; j++) {
    VEC_PUSH(scanner->pair_stack, forgotten);
  }
  for (uint32_t j = 0; j < pair_stored; j++) {
    PairEntry e = {0, 0, 0, 0, 0};
    e.flag = GET();
    if (e.flag & PAIR_FLAG_HASH) {
      uint8_t b0 = GET();
      uint8_t b1 = GET();
      uint8_t b2 = GET();
      e.hash24 = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | b2;
    } else {
      e.table_idx = GET();
    }
    e.indent_floor = GET();
    e.tag_floor = GET();
    VEC_PUSH(scanner->pair_stack, e);
  }

  uint8_t indent_count = GET();
  for (uint8_t j = 0; j < indent_count && i <= length; j++) {
    VEC_PUSH(scanner->indent_length_stack, GET());
  }
  for (uint8_t j = 0; j < indent_count && i <= length; j++) {
    VEC_PUSH(scanner->bullet_stack, GET());
  }

  while (i < length) {
    VEC_PUSH(scanner->section_stack, GET());
  }
}
#undef GET

static bool in_error_recovery(const bool *valid_symbols) {
  return (valid_symbols[LIST_START]         //
          && valid_symbols[BLOCK_TAG_START] //
          && valid_symbols[BLOCK_TAG_END]   //
          && valid_symbols[LIST_END]        //
          && valid_symbols[LISTITEM_END]    //
          && valid_symbols[BULLET]          //
          && valid_symbols[FENCE]           //
          && valid_symbols[SIGNATURE]       //
          && valid_symbols[SECTION_END]     //
          && valid_symbols[ENDOFFILE]       //
          && valid_symbols[TAG_NL]          //
  );
}

static bool dedent(Scanner *scanner, TSLexer *lexer) {
  // Never pop the sentinel, nor a list that belongs outside the current
  // pair (len is unsigned: 0 - 1 wraps).
  if (in_list(scanner)) {
    VEC_POP(scanner->indent_length_stack);
    VEC_POP(scanner->bullet_stack);
  }
  lexer->result_symbol = LIST_END;
  return true;
}

static bool dedent_block_tag(Scanner *scanner, TSLexer *lexer) {
  // VEC_POP(scanner->tag_bracket_stack);
  // VEC_POP(scanner->tag_token_stack);
  if (in_block_tag(scanner))
    VEC_POP(scanner->tag_indent_length_stack);
  lexer->result_symbol = BLOCK_TAG_END;
  return true;
}

static bool indent(Scanner *scanner, TSLexer *lexer, int16_t indent_length,
                   Bullet bullet) {
  VEC_PUSH(scanner->indent_length_stack, indent_length);
  VEC_PUSH(scanner->bullet_stack, bullet);
  lexer->result_symbol = LIST_START;
  return true;
}

static bool indent_block_tag(Scanner *scanner, TSLexer *lexer,
                             int16_t indent_length) {
  VEC_PUSH(scanner->tag_indent_length_stack, indent_length);
  lexer->result_symbol = BLOCK_TAG_START;
  return true;
}

static bool check_token(TSLexer *lexer) {
  return iswalnum(lexer->lookahead) || lexer->lookahead == '_';
}

static bool check_delimiter(TSLexer *lexer) {
  return (lexer->lookahead == '.' || lexer->lookahead == ',' ||
          lexer->lookahead == ':' || lexer->lookahead == ';' ||
          lexer->lookahead == '!' || lexer->lookahead == '?' ||
          lexer->lookahead == '/' || lexer->lookahead == '\\' ||
          lexer->lookahead == '\'' || lexer->lookahead == '"' ||
          lexer->lookahead == '`' || lexer->lookahead == '-' ||
          lexer->lookahead == '+' || lexer->lookahead == '*' ||
          lexer->lookahead == '^' || lexer->lookahead == '%' ||
          lexer->lookahead == '=' || lexer->lookahead == '~' ||
          lexer->lookahead == '@' || lexer->lookahead == '&' ||
          lexer->lookahead == '#' || lexer->lookahead == '$');
}

static bool check_tag_token(TSLexer *lexer) {
  return token_index(lexer->lookahead) >= 0;
}

static bool check_closure(TSLexer *lexer, bool open, bool close) {
  return (open && (lexer->lookahead == '[' || lexer->lookahead == '(' ||
                   lexer->lookahead == '{' || lexer->lookahead == '<')) ||
         (close && (lexer->lookahead == ']' || lexer->lookahead == ')' ||
                    lexer->lookahead == '}' || lexer->lookahead == '>'));
}

static bool check_open_bracket(int32_t c) {
  return c == '[' || c == '(' || c == '{' || c == '<';
}

static bool compare_closure(int32_t open, int32_t close) {
  return (open == '[' && close == ']') || (open == '(' && close == ')') ||
         (open == '{' && close == '}') || (open == '<' && close == '>');
}

static int32_t close_of(int32_t open) {
  return open == '[' ? ']' : open == '(' ? ')' : open == '{' ? '}' : '>';
}

// Is the lookahead the first character of the end of the head being read?
static bool head_end_starts(const Scanner *s, const TSLexer *lexer) {
  return lexer->lookahead ==
         (s->head_form == HEAD_SIGIL ? s->head_sigil : s->head_close);
}

// Called where head_end_starts is true: consumes the end of the head if the
// rest of it is there.
static bool head_end_matches(const Scanner *s, TSLexer *lexer) {
  advance(lexer);
  if (s->head_form == HEAD_SIGIL) {
    if (lexer->lookahead != s->head_close)
      return false;
    advance(lexer);
  } else if (s->head_form == HEAD_BLOCK) {
    if (token_index(lexer->lookahead) < 0)
      return false;
    advance(lexer);
  }
  return true;
}

static bool check_nl(TSLexer *lexer) {
  return (lexer->lookahead == '\r' || lexer->lookahead == '\n');
}

static bool check_eol(TSLexer *lexer) {
  return (lexer->lookahead == '\0' || check_nl(lexer) || lexer->eof(lexer));
}

static bool istabspace(TSLexer *lexer) {
  return (lexer->lookahead == ' ' || lexer->lookahead == '\t');
}

static Bullet getbullet(TSLexer *lexer, bool bullet) {
  bool matched = false;
  bool bracket = false;
  bool has_token = false;
  bool token_underscore = false;
  if (check_delimiter(lexer) || check_closure(lexer, true, true)) {
    bracket = check_closure(lexer, true, true);
    advance(lexer);
    matched = true;
  } else if (check_token(lexer)) {
    has_token = true;
    do {
      token_underscore = lexer->lookahead == '_';
      advance(lexer);
    } while (check_token(lexer));
    if (check_delimiter(lexer) || check_closure(lexer, true, true)) {
      bracket = check_closure(lexer, true, true);
      advance(lexer);
      matched = true;
    }
  }
  if (!matched)
    return NOTABULLET;
  if (bullet)
    return ISABULLET;

  // Empty listitem: the bullet runs straight into the end of the line.
  // Same shapes as is_empty_bullet in scan(): no token or a `_`-terminated
  // one, and never a bracket.
  if (check_eol(lexer))
    return (!bracket && (!has_token || token_underscore)) ? ISABULLET
                                                          : NOTABULLET;

  if (istabspace(lexer)) {
    skip(lexer);
  } else
    return NOTABULLET;

  if (istabspace(lexer)) {
    return ISABULLET;
  }

  return NOTABULLET;
}

// ---------------------------------------------------------------------------
// Line-start tags. Everything that begins with an open bracket is classified
// here in a single forward pass, because once the lexer has advanced it
// cannot back up:
//
//   [ name ...]#      block tag      open, ws, head, ws, close, token
//   [ name ... #]     pair opener    open, ws, head, ws, token, close
//   [# name ]         pair closer    open, token, ws, name, ws, close
//   [# name ... #]    standalone     open, token, ws, ..., ws, token, close,
//   EOL
//
// All emitted tokens are zero-width (mark_end was called before the leading
// whitespace) except _pair_stray_close, which covers the whole `[# name ]`.
// Lookahead uses advance() only: skip() would move the token start.
// ---------------------------------------------------------------------------

typedef enum {
  SHAPE_NONE,
  SHAPE_BLOCK_TAG,
  SHAPE_PAIR_INLINE,
  SHAPE_PAIR_BLOCK,
} OpenShape;

// Advance over a newline (\n, \r\n or \r) and the following indentation.
// Returns false if the next line is blank or the input ends, which ends a
// multi-line tag head (the grammar's _tag_nl does not cross blank lines
// inside a head either, in practice).
static bool lookahead_newline(TSLexer *lexer, int *budget) {
  if (lexer->lookahead == '\r') {
    advance(lexer);
    (*budget)--;
  }
  if (lexer->lookahead == '\n') {
    advance(lexer);
    (*budget)--;
  }
  while (istabspace(lexer)) {
    advance(lexer);
    (*budget)--;
  }
  return !(check_nl(lexer) || lexer->eof(lexer) || lexer->lookahead == '\0');
}

// Called with the lexer just past `<open><ws>`. Reads the head of a
// `[ name ...` tag and reports whether it is a block tag or a pair opener.
// On a pair opener, `name` holds the name and `*token` the closing tag token.
static OpenShape scan_open_shape(TSLexer *lexer, int32_t open, NameAcc *name,
                                 int *token) {
  int budget = PAIR_LOOKAHEAD_CAP;

  // whitespace (and _tag_nl line breaks) before the name
  while (true) {
    if (istabspace(lexer)) {
      advance(lexer);
      budget--;
    } else if (check_nl(lexer)) {
      if (!lookahead_newline(lexer, &budget))
        return SHAPE_NONE;
    } else {
      break;
    }
  }

  if (!is_name_start(lexer->lookahead))
    return SHAPE_NONE;
  name_init(name);
  while (is_name_char(lexer->lookahead) && budget > 0) {
    name_push(name, lexer->lookahead);
    advance(lexer);
    budget--;
  }

  // After the name only a `,`/`;` value list or the closer may follow.
  bool prev_ws = false;
  bool in_values = false;
  while (budget-- > 0) {
    int32_t c = lexer->lookahead;
    if (c == '\0' || lexer->eof(lexer))
      return SHAPE_NONE;

    if (c == '\n' || c == '\r') {
      if (!lookahead_newline(lexer, &budget))
        return SHAPE_NONE;
      prev_ws = true;
      continue;
    }
    if (c == ' ' || c == '\t') {
      prev_ws = true;
      advance(lexer);
      continue;
    }

    if (prev_ws && bracket_index(c) >= 0 && check_closure(lexer, false, true) &&
        compare_closure(open, c)) {
      // ` ]`, the closing bracket of the opening, ends the head. Followed by a
      // tag token it is a block tag. (A closing bracket of another kind is a
      // word, below.)
      advance(lexer);
      return check_tag_token(lexer) ? SHAPE_BLOCK_TAG : SHAPE_NONE;
    }

    if (prev_ws && token_index(c) >= 0) {
      advance(lexer);
      if (check_closure(lexer, false, true) &&
          compare_closure(open, lexer->lookahead)) {
        // ` #]` ends the head of a pair opener
        *token = token_index(c);
        advance(lexer);
        while (istabspace(lexer))
          advance(lexer);
        return check_eol(lexer) ? SHAPE_PAIR_BLOCK : SHAPE_PAIR_INLINE;
      }
      // a value word that starts with a tag token (`->` is one: a sign and a
      // bracket that is not this tag's closing bracket)
      if (!in_values)
        return SHAPE_NONE;
      prev_ws = false;
      continue;
    }

    if (!in_values) {
      if (c != ',' && c != ';')
        return SHAPE_NONE; // `[ foo bar ]` etc.: plain text
      in_values = true;
    }
    prev_ws = false;
    advance(lexer);
  }
  return SHAPE_NONE;
}

static bool scan_pair_open(Scanner *scanner, TSLexer *lexer,
                           const bool *valid_symbols, OpenShape shape,
                           const NameAcc *name, int32_t open_char, int bracket,
                           int token) {
  TSSymbol sym;
  if (shape == SHAPE_PAIR_BLOCK && valid_symbols[PAIR_BLOCK_START])
    sym = PAIR_BLOCK_START;
  else if (valid_symbols[PAIR_OPEN_START])
    sym = PAIR_OPEN_START;
  else
    return false;

  if (scanner->pair_stack->len >= PAIR_MAX_DEPTH)
    return false; // D5: refuse rather than overflow the serialized depth

  PairEntry e = make_entry(name, bracket, token);
  uint32_t ifl = scanner->indent_length_stack->len;
  uint32_t tfl = scanner->tag_indent_length_stack->len;
  e.indent_floor = (uint8_t)(ifl > 255 ? 255 : ifl);
  e.tag_floor = (uint8_t)(tfl > 255 ? 255 : tfl);
  scanner->eol_owed = false;
  scanner->head_form = HEAD_SIGIL;
  scanner->head_sigil = (unsigned char)PAIR_TOKENS[token];
  scanner->head_close = close_of(open_char);
  VEC_PUSH(scanner->pair_stack, e);
  lexer->result_symbol = sym;
  return true;
}

// Called with the lexer just past `<open><token><ws>`.
static bool scan_token_tag(Scanner *scanner, TSLexer *lexer,
                           const bool *valid_symbols, int32_t open,
                           int32_t tag_token) {
  bool want_close = valid_symbols[PAIR_CLOSE_START] ||
                    valid_symbols[PAIR_STRAY_CLOSE] ||
                    valid_symbols[PAIR_IMPLICIT_CLOSE];
  bool prev_space = true;

  while (istabspace(lexer))
    advance(lexer);

  // `[# name ]`: a pair closer
  bool named = is_name_start(lexer->lookahead);
  if (named) {
    NameAcc name;
    name_init(&name);
    while (is_name_char(lexer->lookahead)) {
      name_push(&name, lexer->lookahead);
      advance(lexer);
    }
    prev_space = istabspace(lexer);
    while (istabspace(lexer))
      advance(lexer);

    if (prev_space && want_close && compare_closure(open, lexer->lookahead)) {
      advance(lexer);
      PairEntry closer =
          make_entry(&name, bracket_index(open), token_index(tag_token));
      pair_stack *ps = scanner->pair_stack;
      if (valid_symbols[PAIR_CLOSE_START] && ps->len > 0 &&
          entry_matches(VEC_BACK(ps), closer)) {
        VEC_POP(ps);
        scanner->eol_owed = false;
        lexer->result_symbol = PAIR_CLOSE_START; // zero-width
        return true;
      }
      // Matches an outer opener: close the innermost one implicitly and
      // leave the closer in place (zero-width). The scan repeats until the
      // closer reaches its opener.
      if (valid_symbols[PAIR_IMPLICIT_CLOSE] && ps->len > 1) {
        for (uint32_t j = ps->len - 1; j-- > 0;) {
          if (entry_matches(ps->data[j], closer)) {
            VEC_POP(ps);
            scanner->eol_owed = false;
            lexer->result_symbol = PAIR_IMPLICIT_CLOSE;
            return true;
          }
        }
      }
      if (valid_symbols[PAIR_STRAY_CLOSE]) {
        lexer->mark_end(lexer); // covers the whole `[# name ]`
        scanner->eol_owed = false;
        lexer->result_symbol = PAIR_STRAY_CLOSE;
        return true;
      }
      return false;
    }
  }

  if (!(valid_symbols[STANDALONE_TAG_START] || valid_symbols[SCOPE_TAG_START] ||
        valid_symbols[SCOPE_LINE_TAG_START]))
    return false;

  // A tag has a name, and after the name only a list of values and keys: the
  // text `{{- if .x -}}` (a sign, a blank, words) is text, even though it ends
  // in a sign and a bracket that match.
  if (!named)
    return false;
  bool in_values = false;

  // `[# name ... #]`: the head ends at the first "<ws><sigil><close>" that
  // repeats the opener's sigil and matches its bracket (scanned again as
  // HEAD_END, which remembers both). Other signs and brackets are words. With
  // no such end the text stays plain text.
  bool crossed_nl = false;
  int budget = PAIR_LOOKAHEAD_CAP * 4;
  while (budget-- > 0) {
    int32_t c = lexer->lookahead;
    if (c == '\0' || lexer->eof(lexer))
      return false;

    if (c == '\n' || c == '\r') {
      crossed_nl = true;
      prev_space = true;
      advance(lexer);
      continue;
    }
    if (c == ' ' || c == '\t') {
      prev_space = true;
      advance(lexer);
      continue;
    }

    if (prev_space && token_index(c) >= 0) {
      advance(lexer);
      if (check_closure(lexer, false, true) && c == tag_token &&
          compare_closure(open, lexer->lookahead)) {
        advance(lexer);
        TSSymbol gate;
        if (valid_symbols[STANDALONE_TAG_START] && check_eol(lexer))
          gate = STANDALONE_TAG_START;
        else if (valid_symbols[SCOPE_TAG_START])
          gate = SCOPE_TAG_START;
        else if (valid_symbols[SCOPE_LINE_TAG_START] && !crossed_nl)
          gate = SCOPE_LINE_TAG_START;
        else
          return false;
        scanner->head_form = HEAD_SIGIL;
        scanner->head_sigil = (int16_t)tag_token;
        scanner->head_close = (int16_t)close_of(open);
        lexer->result_symbol = gate;
        return true;
      }
      // any other sign and bracket (`->`, `:)`) is a word of the head
      if (!in_values)
        return false;
      prev_space = false;
      continue;
    }

    if (!in_values) {
      if (c != ',' && c != ';')
        return false; // `{# foo bar #}` etc.: plain text
      in_values = true;
    }
    prev_space = false;
    advance(lexer);
  }
  return false;
}

// Called with the lexer just past `<sigil><open><ws>` of a line tag. Accepts
// the same head shape the grammar does (name, then `,`/`;` values, possibly
// over several lines) and reports whether its closer matches `open`. A value
// word may not start with a close bracket, so the first <ws><close> ends it.
static bool scan_line_tag_head(TSLexer *lexer, int32_t open) {
  int budget = PAIR_LOOKAHEAD_CAP;

  while (true) {
    if (istabspace(lexer)) {
      advance(lexer);
      budget--;
    } else if (check_nl(lexer)) {
      if (!lookahead_newline(lexer, &budget))
        return false;
    } else {
      break;
    }
  }

  if (!is_name_start(lexer->lookahead))
    return false;
  while (is_name_char(lexer->lookahead) && budget > 0) {
    advance(lexer);
    budget--;
  }

  bool prev_ws = false;
  bool in_values = false;
  while (budget-- > 0) {
    int32_t c = lexer->lookahead;
    if (c == '\0' || lexer->eof(lexer))
      return false;
    if (c == '\n' || c == '\r') {
      if (!lookahead_newline(lexer, &budget))
        return false;
      prev_ws = true;
      continue;
    }
    if (c == ' ' || c == '\t') {
      prev_ws = true;
      advance(lexer);
      continue;
    }
    if (prev_ws && check_closure(lexer, false, true) && compare_closure(open, c))
      return true;
    if (!in_values) {
      if (c != ',' && c != ';')
        return false; // `#[ foo bar ]` etc.: plain text
      in_values = true;
    }
    prev_ws = false;
    advance(lexer);
  }
  return false;
}

static bool scan_bracket(Scanner *scanner, TSLexer *lexer,
                         const bool *valid_symbols, int16_t indent_length) {
  // Zero-width tokens sit right at the bracket, not before the whitespace
  // that precedes it (so `pair_open` etc. start at their bracket).
  lexer->mark_end(lexer);
  int32_t open = lexer->lookahead;
  advance(lexer);

  if (istabspace(lexer)) {
    if (!(valid_symbols[BLOCK_TAG_START] || valid_symbols[PAIR_OPEN_START] ||
          valid_symbols[PAIR_BLOCK_START]))
      return false;
    advance(lexer);
    NameAcc name;
    int token = -1;
    OpenShape shape = scan_open_shape(lexer, open, &name, &token);
    switch (shape) {
    case SHAPE_BLOCK_TAG:
      if (valid_symbols[BLOCK_TAG_START]) {
        scanner->head_form = HEAD_BLOCK;
        scanner->head_close = (int16_t)close_of(open);
        return indent_block_tag(scanner, lexer, scanner->block_tag_col);
      }
      return false;
    case SHAPE_PAIR_INLINE:
    case SHAPE_PAIR_BLOCK:
      return scan_pair_open(scanner, lexer, valid_symbols, shape, &name, open,
                            bracket_index(open), token);
    default:
      return false;
    }
  }

  if (check_tag_token(lexer)) {
    if (!(valid_symbols[STANDALONE_TAG_START] ||
          valid_symbols[SCOPE_TAG_START] ||
          valid_symbols[SCOPE_LINE_TAG_START] ||
          valid_symbols[PAIR_CLOSE_START] || valid_symbols[PAIR_STRAY_CLOSE] ||
          valid_symbols[PAIR_IMPLICIT_CLOSE]))
      return false;
    int32_t tag_token = lexer->lookahead;
    advance(lexer);
    // the opener must be followed by a space or tab
    if (!istabspace(lexer))
      return false;
    return scan_token_tag(scanner, lexer, valid_symbols, open, tag_token);
  }

  return false;
}

// At the start of a line, a list item / block tag that is about to end must
// get its LIST_END / LISTITEM_END / BLOCK_TAG_END before any bracket tag on
// that line is considered; otherwise a dedented `[ x ]#` or `[ b #]` would be
// swallowed into the container.
static bool container_end_pending(Scanner *scanner, TSLexer *lexer,
                                  const bool *valid_symbols,
                                  int16_t indent_length, uint32_t skipped) {
  bool list_end = (valid_symbols[LIST_END] || valid_symbols[LISTITEM_END]) &&
                  in_list(scanner) && indent_length <= indent_top(scanner);
  bool tag_end = valid_symbols[BLOCK_TAG_END] && in_block_tag(scanner) &&
                 indent_length <= tag_top(scanner);
  if (!list_end && !tag_end)
    return false;
  return lexer->get_column(lexer) == skipped; // only at the start of a line
}

static bool scan(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols) {
  if (in_error_recovery(valid_symbols))
    return false;

  scanner->is_at_section_start = false;
  uint32_t skipped = 0;

  // eol_owed holds for exactly the scan after the implicit close that set
  // it (the state is restored from the last external token before every
  // scan, so clearing it here persists only if this scan emits a token).
  bool eol_owed = scanner->eol_owed;
  scanner->eol_owed = false;
  if (eol_owed && valid_symbols[ENDOFFILE]) {
    lexer->mark_end(lexer);
    lexer->result_symbol = ENDOFFILE; // zero-width _eol for the text run
    return true;
  }

  // End of a line_tag that has no explicit terminator: succeed only when the
  // rest of the line is blank. Zero-width, so the newline (or EOF) is left
  // for the enclosing paragraph / title / body's own _eol.
  //
  // This is only ever valid mid-line (after a tag_end or a body word), so
  // skipping the trailing blanks here cannot disturb the line-start logic
  // below. `skipped` is still kept in step so get_column() == skipped stays
  // meaningful if we fall through.
  if (valid_symbols[LINE_TAG_EOL]) {
    while (istabspace(lexer)) {
      skip(lexer);
      skipped++;
    }
    if (check_eol(lexer)) {   // '\n', '\r', '\0' or EOF
      lexer->mark_end(lexer); // token ends here: nothing consumed
      lexer->result_symbol = LINE_TAG_EOL;
      return true;
    }
    // Not at end of line: fall through to the normal scan.
  }

  // Handle explicit tag newlines with active list-indent checks.
  //
  // TAG_VALUE_NL and TAG_NL both consume the line break(s) and the following
  // indentation. When both are valid (right after a word of a multi-line
  // value), peek at the next line: if it starts with a tag closer (`]`,
  // `#]`, `]#`, ...) it is TAG_NL (the start of tag_close), otherwise the
  // value continues and it is TAG_VALUE_NL.
  // The end of the head of a scope tag or a pair opener (HEAD_END_BARE on a
  // line of its own, after a line break; HEAD_END after a blank): the sigil and
  // the bracket of the opening, and nothing else.
  bool in_head = scanner->head_form != HEAD_NONE;
  if (in_head && valid_symbols[HEAD_END_BARE]) {
    if (head_end_starts(scanner, lexer)) {
      if (!head_end_matches(scanner, lexer))
        return false; // a word that starts like the end
      lexer->mark_end(lexer);
      scanner->head_form = HEAD_NONE;
      lexer->result_symbol = HEAD_END_BARE;
      return true;
    }
  }
  if (valid_symbols[TAG_NL] || valid_symbols[TAG_VALUE_NL] ||
      (in_head && valid_symbols[HEAD_END])) {
    // In a head the blanks are part of the end (` #}`) and of the line break
    // token alike; anywhere else they are skipped, they belong to neither.
    bool head = in_head && valid_symbols[HEAD_END];
    uint32_t blanks = 0;
    while (istabspace(lexer)) {
      if (head)
        advance(lexer);
      else
        skip(lexer); // not part of the token (and not of any zero-width one)
      skipped++;
      blanks++;
    }
    if (head && !check_nl(lexer)) {
      if (blanks > 0 && head_end_starts(scanner, lexer) &&
          head_end_matches(scanner, lexer)) {
        lexer->mark_end(lexer);
        scanner->head_form = HEAD_NONE;
        lexer->result_symbol = HEAD_END;
        return true;
      }
      return false;
    }
    if (check_nl(lexer)) {
      int16_t indent_length = 0;
      bool blank_only = false;
      // consume this line break, plus any whitespace-only lines after it
      while (check_nl(lexer)) {
        if (lexer->lookahead == '\r')
          advance(lexer);
        if (lexer->lookahead == '\n')
          advance(lexer);
        indent_length = 0;
        while (true) {
          if (lexer->lookahead == ' ') {
            indent_length++;
          } else if (lexer->lookahead == '\t') {
            indent_length += 8;
          } else {
            break;
          }
          advance(lexer);
        }
        // A value may not run into a blank line (same as the opener
        // lookahead in scan_open_shape); a tag closer may follow one.
        if (check_nl(lexer))
          blank_only = true;
      }
      lexer->mark_end(lexer);

      // A multi-line tag head may not dedent out of its list item or block
      // tag (only those in the current indentation root count).
      if (in_list(scanner)) {
        if (indent_length < indent_top(scanner)) {
          return false;
        }
      } else if (in_block_tag(scanner)) {
        if (indent_length < tag_top(scanner)) {
          return false;
        }
      }

      bool closer = false;
      if (head) {
        // only the end that belongs to the opening ends the head
        closer = head_end_starts(scanner, lexer) && head_end_matches(scanner, lexer);
      } else if (check_closure(lexer, false, true)) {
        closer = true; // `]`, `]#`
      } else if (check_tag_token(lexer)) {
        advance(lexer); // peek only: the token already ends at mark_end
        closer = check_closure(lexer, false, true); // `#]`
      }

      if (!closer && !blank_only && valid_symbols[TAG_VALUE_NL]) {
        scanner->eol_owed = false;
        lexer->result_symbol = TAG_VALUE_NL;
        return true;
      }
      if (valid_symbols[TAG_NL]) {
        scanner->eol_owed = false;
        lexer->result_symbol = TAG_NL;
        return true;
      }
      return false;
    }
  }

  // - Section ends
  int16_t indent_length = 0;
  lexer->mark_end(lexer);
  while (true) {
    if (lexer->lookahead == ' ') {
      indent_length++;
    } else if (lexer->lookahead == '\t') {
      indent_length += 8;
    } else if (lexer->lookahead == '\0') {
      // An unclosed pair ends with the input.
      if (valid_symbols[PAIR_IMPLICIT_CLOSE] && scanner->pair_stack->len > 0) {
        VEC_POP(scanner->pair_stack);
        lexer->result_symbol = PAIR_IMPLICIT_CLOSE;
        return true;
      }
      //
      if (valid_symbols[LIST_END]) {
        lexer->result_symbol = LIST_END;
        //
      } else if (valid_symbols[BLOCK_TAG_END]) {
        lexer->result_symbol = BLOCK_TAG_END;
        //
      } else if (valid_symbols[SECTION_END]) {
        VEC_CLEAR(scanner->pair_stack); // a pair cannot span headings
        lexer->result_symbol = SECTION_END;
        //
      } else if (valid_symbols[ENDOFFILE]) {
        lexer->result_symbol = ENDOFFILE;
        //
      } else
        return false;

      return true;
    } else {
      break;
    }
    skip(lexer);
    skipped++;
  }

  // Only a token at the very start of a line may implicitly close a pair
  // before a heading (checked at the signature test below).
  bool at_line_start = lexer->get_column(lexer) == skipped;

  // - Tags that start with an open bracket (see scan_bracket)
  int16_t newlines = 0;
  if ((valid_symbols[STANDALONE_TAG_START] || valid_symbols[BLOCK_TAG_START] ||
       valid_symbols[SCOPE_TAG_START] || valid_symbols[SCOPE_LINE_TAG_START] ||
       valid_symbols[PAIR_OPEN_START] || valid_symbols[PAIR_BLOCK_START] ||
       valid_symbols[PAIR_CLOSE_START] || valid_symbols[PAIR_STRAY_CLOSE] ||
       valid_symbols[PAIR_IMPLICIT_CLOSE]) &&
      check_closure(lexer, true, false) &&
      !container_end_pending(scanner, lexer, valid_symbols, indent_length,
                             skipped)) {
    // A block tag that opens right after a list bullet takes the indent of
    // that line (the list item), not the column of its own bracket.
    scanner->block_tag_col = (!at_line_start && in_list(scanner))
                                 ? indent_top(scanner)
                                 : (int16_t)lexer->get_column(lexer);
    return scan_bracket(scanner, lexer, valid_symbols, indent_length);
  }

  if (valid_symbols[LIST_END] || valid_symbols[LISTITEM_END] ||
      valid_symbols[BLOCK_TAG_END]) {
    while (true) {
      if (lexer->lookahead == ' ') {
        indent_length++;
      } else if (lexer->lookahead == '\t') {
        indent_length += 8;
      } else if (lexer->lookahead == '\0') {
        // Protect against stack underflow
        if (valid_symbols[BLOCK_TAG_END] && in_block_tag(scanner)) {
          return dedent_block_tag(scanner, lexer);
        }
        if (valid_symbols[LIST_END]) {
          return dedent(scanner, lexer);
        }
        break; // Escape if neither applies
      } else if (lexer->lookahead == '\n') {
        if (++newlines > 1) {
          if (valid_symbols[BLOCK_TAG_END] && in_block_tag(scanner)) {
            return dedent_block_tag(scanner, lexer);
          }
          if (valid_symbols[LIST_END]) {
            return dedent(scanner, lexer);
          }
        }
        indent_length = 0;
      } else {
        break;
      }
      skip(lexer);
    }

    if (valid_symbols[BLOCK_TAG_END] && in_block_tag(scanner)) {
      if (indent_length <= tag_top(scanner)) {
        return dedent_block_tag(scanner, lexer);
      }
    }

    if (indent_length < indent_top(scanner)) {
      return dedent(scanner, lexer);
    } else if (indent_length == indent_top(scanner)) {
      if ((int16_t)getbullet(lexer, false) == bullet_top(scanner)) {
        lexer->result_symbol = LISTITEM_END;
        return true;
      }
      return dedent(scanner, lexer);
    } else if (scanner->base_indent != -1 &&
               indent_length == scanner->base_indent + 2) {
      scanner->is_at_section_start = true;
    }
  }

  // Zero-width lookahead for
  // block fences, listitem bullets, and heading signatures
  int16_t segments = 0;
  uint32_t fence_col = lexer->get_column(lexer);
  int32_t fence_char = lexer->lookahead;
  int16_t fence_width = 0;
  bool fenceable = true;
  bool segmentable = true;
  // Line-tag candidate: the run is exactly `<sigil><open>` (e.g. `#[`).
  int32_t run_first = lexer->lookahead;
  int32_t run_open = 0;
  int run_len = 0;
  // Whether the last segment's delimiter was a bracket (empty bullets
  // may not use one).
  bool last_seg_bracket = false;
  // Whether the last segment had a token, and if so whether it ended in `_`
  // (pure anonymous text, i.e. a data key like `key_:`).
  bool last_seg_token = false;
  bool last_token_underscore = false;
  // Mid-line, a line-tag gate sits at its sigil, not before the blanks that
  // precede it (like the bracket tags in scan_bracket). At a line start the
  // other zero-width tokens here (bullet, signature, ...) keep their end
  // before the indentation, so leave mark_end alone there.
  if (!at_line_start && valid_symbols[LINE_TAG_START] &&
      token_index(run_first) >= 0)
    lexer->mark_end(lexer);

  while (check_token(lexer) || check_delimiter(lexer) ||
         check_closure(lexer, true, true) //
  ) {
    if (check_token(lexer)) {
      while (check_token(lexer)) {
        last_token_underscore = lexer->lookahead == '_';
        skip(lexer);
      }
      last_seg_token = true;
      segmentable = false;
      fenceable = false;
      run_len = 99;
    } else if (check_delimiter(lexer)) {
      run_len++;
      segments += 1;
      segmentable = true;
      last_seg_bracket = false;
      if (fenceable && lexer->lookahead == fence_char) {
        fence_width += 1;
      } else {
        fenceable = false;
      }
      skip(lexer);
    } else if (check_closure(lexer, true, true)) {
      if (run_len == 1)
        run_open = lexer->lookahead;
      run_len++;
      segments += 1;
      segmentable = true;
      last_seg_bracket = true;
      fenceable = false;
      skip(lexer);
    }
  }
  bool line_tag_shape = run_len == 2 && token_index(run_first) >= 0 &&
                        check_open_bracket(run_open);
  bool ws_after_run = false;

  // Indent of the line that holds this token. Mid-line, directly after a
  // list bullet, `indent_length` only counts the blanks that separate the
  // bullet from its contents, so the line starts where the list item does.
  int16_t line_indent = (!at_line_start && in_list(scanner))
                            ? indent_top(scanner)
                            : indent_length;
  // A fence that follows a bullet starts at the column of the item's text. Its
  // closer may line up with that column as well as with the bullet: see the
  // fence stacks.
  int16_t fence_open_col = at_line_start ? line_indent : (int16_t)fence_col;

  bool fence_looked_ahead = false;
  // Decide "delimiters run straight into the line end" BEFORE the fence
  // lookahead below can skip the single space after them.
  bool delims_hit_eol = lexer->lookahead == '\n' || lexer->lookahead == '\r';
  // Empty listitem: a single segment that runs straight into the end of the
  // line or input. Its value, if any, is the sublist on the following lines.
  // Only two shapes qualify, so ordinary prose is never mistaken for one:
  //   `-`      no token at all
  //   `key_:`  a token that is pure anonymous text (ends in `_`)
  // A wrapped paragraph line like `Done.` (token `Done`, no `_`) stays text,
  // and bracket bullets are excluded because a line holding only `]` or `}`
  // is the closer of a multi-line tag head.
  bool is_empty_bullet = segmentable && segments == 1 && !last_seg_bracket &&
                         (!last_seg_token || last_token_underscore) &&
                         check_eol(lexer);
  if (fenceable) {
    bool valid_fence_suffix = false;
    if (lexer->lookahead == '\n' || lexer->lookahead == '\r' ||
        lexer->lookahead == '\0') {
      valid_fence_suffix = true;
    } else if (istabspace(lexer)) {
      skip(lexer);
      fence_looked_ahead = true;
      if (istabspace(lexer)) {
        valid_fence_suffix = true;
      }
    }

    // `newlines` > 0: the lookahead above skipped blank lines, and this
    // token is zero-width before them. Let the grammar take the newlines
    // first; the fence is seen again at the start of its own line.
    if (valid_symbols[FENCE] && fence_width >= 3 && newlines == 0) {
      if (scanner->fence_indent_stack->len == 0) {
        if (valid_fence_suffix) {
          VEC_PUSH(scanner->fence_indent_stack, line_indent);
          VEC_PUSH(scanner->fence_col_stack, fence_open_col);
          VEC_PUSH(scanner->fence_width_stack, fence_width);
          VEC_PUSH(scanner->fence_char_stack, fence_char);
          lexer->result_symbol = FENCE;
          return true;
        }
      } else if (VEC_BACK(scanner->fence_indent_stack) <= line_indent &&
                 line_indent <= VEC_BACK(scanner->fence_col_stack) &&
                 VEC_BACK(scanner->fence_width_stack) == fence_width &&
                 VEC_BACK(scanner->fence_char_stack) == fence_char) {
        VEC_POP(scanner->fence_indent_stack);
        VEC_POP(scanner->fence_col_stack);
        VEC_POP(scanner->fence_width_stack);
        VEC_POP(scanner->fence_char_stack);
        lexer->result_symbol = FENCE;
        return true;
      }
    }
  }

  if (!segmentable)
    return false;

  bool is_bullet = false;
  bool is_signature = false;

  // if (lexer->lookahead != '\n' && lexer->lookahead != '\r') {
  if (!delims_hit_eol) {
    bool has_second_space = false;
    if (fence_looked_ahead || istabspace(lexer)) {
      ws_after_run = true;
      if (!fence_looked_ahead)
        skip(lexer);
      if (istabspace(lexer)) {
        has_second_space = true;
      }
    }

    is_bullet = (segments == 1 && has_second_space);
    is_signature = (segments > 0 && !has_second_space);
  }

  // An empty bullet only exists where a list may start or continue;
  // otherwise fall through (e.g. a lone `*` at EOF can still be a heading).
  if (is_empty_bullet && newlines == 0 && at_line_start) {
    if (valid_symbols[BULLET] && indent_length == indent_top(scanner)) {
      lexer->result_symbol = BULLET;
      return true;
    }
    if (valid_symbols[LIST_START] && indent_length > indent_top(scanner))
      return indent(scanner, lexer, indent_length, ISABULLET);
  }

  if (is_bullet && newlines == 0 && at_line_start) {
    if (valid_symbols[BULLET]                   //
        && indent_length == indent_top(scanner) //
    ) {
      lexer->result_symbol = BULLET;
      return true;

    } else if (valid_symbols[LIST_START]              //
               && indent_length > indent_top(scanner) //
    ) {
      return indent(scanner, lexer, indent_length, ISABULLET);
    }
    return false;
  }

  if (is_signature) {
    // NOTE: base_indent is only committed on the returns below (scanner
    // state must change only when a token is emitted).
    int16_t base_indent = scanner->base_indent;
    if (base_indent == -1) {
      base_indent = indent_length >= 2 ? indent_length - 2 : 0;
    }

    if (indent_length == base_indent + 2) {
      // An unclosed pair cannot run into a heading: close it implicitly
      // here. The text run that held it is owed its end of line.
      if (valid_symbols[PAIR_IMPLICIT_CLOSE] && scanner->pair_stack->len > 0 &&
          at_line_start && scanner->fence_indent_stack->len == 0) {
        VEC_POP(scanner->pair_stack);
        scanner->eol_owed = true;
        lexer->result_symbol = PAIR_IMPLICIT_CLOSE;
        return true;
      }
      if (scanner->is_at_section_start && valid_symbols[LIST_END]) {
        scanner->base_indent = base_indent;
        return dedent(scanner, lexer);
      } else if (scanner->is_at_section_start && valid_symbols[BLOCK_TAG_END]) {
        scanner->base_indent = base_indent;
        return dedent_block_tag(scanner, lexer);
      }

      if (valid_symbols[SECTION_END]                      //
          && segments <= VEC_BACK(scanner->section_stack) //
      ) {
        scanner->base_indent = base_indent;
        VEC_POP(scanner->section_stack);
        VEC_CLEAR(scanner->pair_stack); // a pair cannot span headings
        lexer->result_symbol = SECTION_END;
        return true;

      } else if (valid_symbols[SIGNATURE]) {
        scanner->base_indent = base_indent;
        VEC_PUSH(scanner->section_stack, segments);
        VEC_CLEAR(scanner->pair_stack); // a pair cannot span headings
        lexer->result_symbol = SIGNATURE;
        return true;
      }
    }
  }

  // Line tag `<sigil><open><ws>head<ws><close>`. Checked last, reusing the
  // lookahead above (which already consumed `<sigil><open><ws>`), so a
  // heading, bullet or fence on the same characters always wins first.
  if (valid_symbols[LINE_TAG_START] && line_tag_shape && ws_after_run &&
      newlines == 0 && scan_line_tag_head(lexer, run_open)) {
    scanner->head_form = HEAD_BRACKET;
    scanner->head_close = (int16_t)close_of(run_open);
    lexer->result_symbol = LINE_TAG_START; // zero-width (mark_end above)
    return true;
  }

  return false;
}

void *tree_sitter_fey_external_scanner_create() {
  Scanner *scanner = (Scanner *)calloc(1, sizeof(Scanner));
  scanner->indent_length_stack = (stack *)calloc(1, sizeof(stack));
  scanner->bullet_stack = (stack *)calloc(1, sizeof(stack));
  scanner->section_stack = (stack *)calloc(1, sizeof(stack));

  scanner->tag_indent_length_stack = (stack *)calloc(1, sizeof(stack));

  scanner->fence_indent_stack = (stack *)calloc(1, sizeof(stack));
  scanner->fence_col_stack = (stack *)calloc(1, sizeof(stack));
  scanner->fence_width_stack = (stack *)calloc(1, sizeof(stack));
  scanner->fence_char_stack = (stack *)calloc(1, sizeof(stack));

  scanner->pair_stack = (pair_stack *)calloc(1, sizeof(pair_stack));

  deserialize(scanner, NULL, 0);
  return scanner;
}

bool tree_sitter_fey_external_scanner_scan(                  //
    void *payload, TSLexer *lexer, const bool *valid_symbols //
) {
  Scanner *scanner = (Scanner *)payload;
  return scan(scanner, lexer, valid_symbols);
}

unsigned tree_sitter_fey_external_scanner_serialize( //
    void *payload, char *buffer                      //
) {
  Scanner *scanner = (Scanner *)payload;
  return serialize(scanner, buffer);
}

void tree_sitter_fey_external_scanner_deserialize(     //
    void *payload, const char *buffer, unsigned length //
) {
  Scanner *scanner = (Scanner *)payload;
  deserialize(scanner, buffer, length);
}

void tree_sitter_fey_external_scanner_destroy(void *payload) {
  Scanner *scanner = (Scanner *)payload;
  VEC_FREE(scanner->indent_length_stack);
  VEC_FREE(scanner->bullet_stack);
  VEC_FREE(scanner->section_stack);

  VEC_FREE(scanner->tag_indent_length_stack);

  VEC_FREE(scanner->fence_indent_stack);
  VEC_FREE(scanner->fence_col_stack);
  VEC_FREE(scanner->fence_width_stack);
  VEC_FREE(scanner->fence_char_stack);

  VEC_FREE(scanner->pair_stack);

  free(scanner->fence_indent_stack);
  free(scanner->fence_col_stack);
  free(scanner->fence_width_stack);
  free(scanner->fence_char_stack);

  free(scanner->tag_indent_length_stack);

  free(scanner->pair_stack);

  free(scanner->indent_length_stack);
  free(scanner->bullet_stack);
  free(scanner->section_stack);
  free(scanner);
}
