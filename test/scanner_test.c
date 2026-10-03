// Unit tests for the pair-tag parts of the external scanner.
//
//   cc -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined
//      -Isrc test/scanner_test.c -o /tmp/scanner_test && /tmp/scanner_test
//
// The scanner is #included so its static functions are reachable.
#include "../src/scanner.c"

#include <stdarg.h>

static int failures = 0;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                     \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

// ---- mock lexer -------------------------------------------------------------

typedef struct {
  TSLexer base;
  const char *src;
  size_t pos, start, end;
  bool marked;
} Mock;

static void m_advance(TSLexer *l, bool skip_) {
  Mock *m = (Mock *)l;
  if (m->src[m->pos])
    m->pos++;
  if (skip_)
    m->start = m->pos;
  l->lookahead = (unsigned char)m->src[m->pos];
}
static void m_mark_end(TSLexer *l) {
  Mock *m = (Mock *)l;
  m->end = m->pos;
  m->marked = true;
}
static uint32_t m_col(TSLexer *l) {
  Mock *m = (Mock *)l;
  size_t c = 0, p = m->pos;
  while (p > 0 && m->src[p - 1] != '\n') {
    p--;
    c++;
  }
  return (uint32_t)c;
}
static bool m_eof(const TSLexer *l) {
  const Mock *m = (const Mock *)l;
  return m->src[m->pos] == '\0';
}
static bool m_range(const TSLexer *l) {
  (void)l;
  return false;
}
static void m_log(const TSLexer *l, const char *f, ...) {
  (void)l;
  (void)f;
}

static void mock_init(Mock *m, const char *src) {
  memset(m, 0, sizeof *m);
  m->src = src;
  m->base.lookahead = (unsigned char)src[0];
  m->base.advance = m_advance;
  m->base.mark_end = m_mark_end;
  m->base.get_column = m_col;
  m->base.is_at_included_range_start = m_range;
  m->base.eof = m_eof;
  m->base.log = m_log;
}

// Run one scan at offset 0 of `src` with only `syms` valid. Returns the
// emitted symbol or -1. *len receives the token length.
static int scan_one(Scanner *s, const char *src, const int *syms, int nsyms,
                    size_t *len) {
  bool valid[PAIR_STRAY_CLOSE + 1] = {0};
  for (int k = 0; k < nsyms; k++)
    valid[syms[k]] = true;
  Mock m;
  mock_init(&m, src);
  if (!scan(s, &m.base, valid))
    return -1;
  size_t end = m.marked ? m.end : m.pos;
  size_t start = m.start <= end ? m.start : end;
  if (len)
    *len = end - start;
  return m.base.result_symbol;
}

static const int INLINE_SYMS[] = {PAIR_OPEN_START, PAIR_CLOSE_START,
                                  PAIR_STRAY_CLOSE, TAG_NL};
#define NINLINE 4

// ---- helpers ----------------------------------------------------------------

static Scanner *fresh(void) {
  return (Scanner *)tree_sitter_fey_external_scanner_create();
}

static PairEntry entry_for(const char *name, int bracket, int token) {
  NameAcc n;
  name_init(&n);
  for (const char *p = name; *p; p++)
    name_push(&n, *p);
  return make_entry(&n, bracket, token);
}

static bool entry_eq(PairEntry a, PairEntry b) {
  if (a.flag != b.flag)
    return false;
  if (a.flag == PAIR_FORGOTTEN)
    return true;
  return (a.flag & PAIR_FLAG_HASH) ? a.hash24 == b.hash24
                                   : a.table_idx == b.table_idx;
}

// ---- tests ------------------------------------------------------------------

static void test_table(void) {
  for (int k = 1; k < HTML_NAMES_COUNT; k++)
    CHECK(strcmp(HTML_NAMES[k - 1], HTML_NAMES[k]) < 0, "table not sorted at %d",
          k);
  for (int k = 0; k < HTML_NAMES_COUNT; k++)
    CHECK(html_name_lookup(HTML_NAMES[k]) == k, "lookup %s", HTML_NAMES[k]);
  CHECK(html_name_lookup("wbr") >= 0 && html_name_lookup("xmp") >= 0,
        "last names reachable");
  CHECK(html_name_lookup("nope") < 0, "unknown name");
}

static void test_classify(void) {
  PairEntry b = entry_for("b", 0, 1), B = entry_for("B", 0, 1);
  CHECK(!(b.flag & PAIR_FLAG_HASH), "b uses the table");
  CHECK(entry_eq(b, B), "case folding (D1)");
  CHECK(!entry_eq(b, entry_for("b", 1, 1)), "bracket stored (D2)");
  CHECK(!entry_eq(b, entry_for("b", 0, 2)), "token stored (D2)");
  PairEntry w = entry_for("my-widget", 0, 1);
  CHECK(w.flag & PAIR_FLAG_HASH, "custom name hashed");
  CHECK(w.hash24 <= 0xFFFFFF, "24-bit");
  CHECK(entry_eq(w, entry_for("MY-WIDGET", 0, 1)), "hash folds case");
  // the longest flag must stay below the FORGOTTEN marker
  PairEntry hi = entry_for("zzz", 3, 18);
  CHECK(hi.flag == 0x97, "max flag is 0x97, got %#x", hi.flag);
  // long names: same 31-char prefix must still differ
  char a[80], c[80];
  memset(a, 'x', 70);
  a[70] = 0;
  memcpy(c, a, 71);
  c[69] = 'y';
  CHECK(!entry_eq(entry_for(a, 0, 1), entry_for(c, 0, 1)),
        "whole name is hashed");
}

static void test_collision(void) {
  // Find two distinct names with equal h24 and check they "match" (the only
  // effect of a collision) without any crash.
  enum { N = 1 << 14 };
  static uint32_t seen_h[N];
  static int seen_k[N];
  static bool used[N];
  char n1[16], n2[16];
  bool found = false;
  for (int k = 0; k < 200000 && !found; k++) {
    snprintf(n1, sizeof n1, "n%d", k);
    PairEntry e = entry_for(n1, 0, 1);
    uint32_t slot = e.hash24 & (N - 1);
    while (used[slot] && seen_h[slot] != e.hash24)
      slot = (slot + 1) & (N - 1);
    if (used[slot]) {
      snprintf(n2, sizeof n2, "n%d", seen_k[slot]);
      found = true;
      break;
    }
    if (k < N / 2) {
      used[slot] = true;
      seen_h[slot] = e.hash24;
      seen_k[slot] = k;
    }
  }
  if (!found) {
    printf("  (no 24-bit collision found in search space; skipped)\n");
    return;
  }
  printf("  collision: %s / %s\n", n1, n2);
  Scanner *s = fresh();
  char open_src[64], close_src[64];
  snprintf(open_src, sizeof open_src, "[ %s #]x", n1);
  snprintf(close_src, sizeof close_src, "[# %s ]", n2);
  CHECK(scan_one(s, open_src, INLINE_SYMS, NINLINE, NULL) == PAIR_OPEN_START,
        "collision open");
  CHECK(scan_one(s, close_src, INLINE_SYMS, NINLINE, NULL) == PAIR_CLOSE_START,
        "colliding closer matches (documented false positive)");
  CHECK(s->pair_stack->len == 0, "popped");
  tree_sitter_fey_external_scanner_destroy(s);
}

static void test_scan_shapes(void) {
  Scanner *s = fresh();
  size_t len = 99;

  CHECK(scan_one(s, "[ b #]bold", INLINE_SYMS, NINLINE, &len) ==
            PAIR_OPEN_START,
        "inline opener");
  CHECK(len == 0, "opener is zero-width");
  CHECK(s->pair_stack->len == 1, "pushed");

  CHECK(scan_one(s, "[# i ]", INLINE_SYMS, NINLINE, &len) == PAIR_STRAY_CLOSE,
        "mismatch -> stray");
  CHECK(len == 6, "stray covers `[# i ]`, got %zu", len);
  CHECK(s->pair_stack->len == 1, "stray does not pop");

  CHECK(scan_one(s, "{# b }", INLINE_SYMS, NINLINE, NULL) == PAIR_STRAY_CLOSE,
        "other bracket -> stray");
  CHECK(scan_one(s, "[# B ]", INLINE_SYMS, NINLINE, &len) == PAIR_CLOSE_START,
        "matching closer");
  CHECK(len == 0, "closer start is zero-width");
  CHECK(s->pair_stack->len == 0, "popped");

  int no_stray[] = {PAIR_OPEN_START, PAIR_CLOSE_START};
  CHECK(scan_one(s, "[# x ]", no_stray, 2, NULL) == -1,
        "no match, stray invalid -> false");

  // text that is not a tag
  CHECK(scan_one(s, "[ foo ] x", INLINE_SYMS, NINLINE, NULL) == -1, "[ foo ]");
  CHECK(scan_one(s, "[ foo bar #]", INLINE_SYMS, NINLINE, NULL) == -1,
        "no delimiter between name and word");
  CHECK(scan_one(s, "[ 1 #]", INLINE_SYMS, NINLINE, NULL) == -1, "[ 1 #]");
  CHECK(scan_one(s, "[foo#]", INLINE_SYMS, NINLINE, NULL) == -1, "[foo#]");
  CHECK(scan_one(s, "[ a #}", INLINE_SYMS, NINLINE, NULL) == -1,
        "mismatched opener brackets");
  CHECK(s->pair_stack->len == 0, "failed scans leave the stack alone");

  // block vs inline
  int block_syms[] = {PAIR_OPEN_START, PAIR_BLOCK_START, BLOCK_TAG_START,
                      STANDALONE_TAG_START};
  CHECK(scan_one(s, "[ div #]  \nx", block_syms, 4, NULL) == PAIR_BLOCK_START,
        "block opener");
  CHECK(scan_one(s, "[ div #] x", block_syms, 4, NULL) == PAIR_OPEN_START,
        "inline opener at line start");
  CHECK(scan_one(s, "[ note ]#  x", block_syms, 4, NULL) == BLOCK_TAG_START,
        "block tag");
  CHECK(scan_one(s, "[# alone #]\n", block_syms, 4, NULL) ==
            STANDALONE_TAG_START,
        "standalone");
  CHECK(scan_one(s, "[# a # b ]\n", block_syms, 4, NULL) == -1,
        "standalone needs sigil before the bracket");
  CHECK(scan_one(s, "[ a,\n  x #]y", INLINE_SYMS, NINLINE, NULL) ==
            PAIR_OPEN_START,
        "multi-line head");
  CHECK(scan_one(s, "[ a,\n\n x #]y", INLINE_SYMS, NINLINE, NULL) == -1,
        "head does not cross a blank line");
  tree_sitter_fey_external_scanner_destroy(s);
}

static void test_depth_limit(void) {
  Scanner *s = fresh();
  for (int k = 0; k < 300; k++)
    scan_one(s, "[ x #]y", INLINE_SYMS, NINLINE, NULL);
  CHECK(s->pair_stack->len == PAIR_MAX_DEPTH, "depth capped at 255, got %u",
        s->pair_stack->len);
  CHECK(scan_one(s, "[ x #]y", INLINE_SYMS, NINLINE, NULL) == -1,
        "opener refused past 255 (D5)");
  tree_sitter_fey_external_scanner_destroy(s);
}

// Fill a scanner with `depth` pair entries (alternating table / hash names
// when `all_hash` is false) plus some list and section state.
static void fill(Scanner *s, int depth, bool all_hash, int lists,
                 int sections) {
  for (int k = 0; k < depth; k++) {
    char name[24];
    if (all_hash || k % 2)
      snprintf(name, sizeof name, "custom-%d", k);
    else
      snprintf(name, sizeof name, "%s", HTML_NAMES[k % HTML_NAMES_COUNT]);
    PairEntry e = entry_for(name, k % 4, k % 19);
    VEC_PUSH(s->pair_stack, e);
  }
  for (int k = 0; k < lists; k++) {
    VEC_PUSH(s->indent_length_stack, (int16_t)(k * 4));
    VEC_PUSH(s->bullet_stack, ISABULLET);
  }
  for (int k = 0; k < sections; k++)
    VEC_PUSH(s->section_stack, (int16_t)(k % 7 + 1));
  VEC_PUSH(s->tag_indent_length_stack, 2);
  VEC_PUSH(s->fence_indent_stack, 0);
  VEC_PUSH(s->fence_width_stack, 3);
  VEC_PUSH(s->fence_char_stack, '`');
  s->base_indent = 0;
}

static void round_trip(int depth, bool all_hash, int lists, int sections) {
  Scanner *a = fresh(), *b = fresh();
  fill(a, depth, all_hash, lists, sections);
  char buf[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned n = serialize(a, buf);
  CHECK(n <= SER_MAX, "depth %d: %u bytes", depth, n);
  deserialize(b, buf, n);

  CHECK(b->pair_stack->len == a->pair_stack->len, "depth %d: len %u vs %u",
        depth, b->pair_stack->len, a->pair_stack->len);
  uint32_t forgotten = 0;
  for (uint32_t j = 0; j < b->pair_stack->len; j++) {
    PairEntry got = b->pair_stack->data[j];
    if (got.flag == PAIR_FORGOTTEN) {
      forgotten++;
      CHECK(forgotten == j + 1, "depth %d: forgotten entries must be oldest",
            depth);
    } else {
      CHECK(entry_eq(got, a->pair_stack->data[j]), "depth %d: entry %u", depth,
            j);
    }
  }
  // innermost entry must always survive
  if (depth > 0)
    CHECK(VEC_BACK(b->pair_stack).flag != PAIR_FORGOTTEN,
          "depth %d: innermost kept", depth);

  // everything else must survive untouched
  CHECK(b->indent_length_stack->len == a->indent_length_stack->len,
        "depth %d: lists", depth);
  for (uint32_t j = 0; j < a->indent_length_stack->len; j++)
    CHECK(b->indent_length_stack->data[j] == a->indent_length_stack->data[j],
          "depth %d: indent %u", depth, j);
  CHECK(b->section_stack->len == a->section_stack->len,
        "depth %d: sections %u vs %u", depth, b->section_stack->len,
        a->section_stack->len);
  for (uint32_t j = 0; j < a->section_stack->len; j++)
    CHECK(b->section_stack->data[j] == a->section_stack->data[j],
          "depth %d: section %u", depth, j);
  CHECK(b->tag_indent_length_stack->len == 1 && b->fence_width_stack->len == 1,
        "depth %d: tag/fence", depth);

  // re-serializing the restored state is stable
  char buf2[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned n2 = serialize(b, buf2);
  CHECK(n2 == n && memcmp(buf, buf2, n) == 0, "depth %d: stable", depth);

  printf("  depth %3d %-9s lists %3d sections %3d: %4u bytes, %3u forgotten\n",
         depth, all_hash ? "all-hash" : "mixed", lists, sections, n, forgotten);
  tree_sitter_fey_external_scanner_destroy(a);
  tree_sitter_fey_external_scanner_destroy(b);
}

static void test_forgotten_matches_any(void) {
  Scanner *s = fresh();
  PairEntry f = {PAIR_FORGOTTEN, 0, 0};
  VEC_PUSH(s->pair_stack, f);
  CHECK(scan_one(s, "{$ anything }", INLINE_SYMS, NINLINE, NULL) ==
            PAIR_CLOSE_START,
        "forgotten matches any closer");
  CHECK(s->pair_stack->len == 0, "and is popped");
  tree_sitter_fey_external_scanner_destroy(s);
}

static void test_deserialize_edge(void) {
  Scanner *s = fresh();
  fill(s, 5, false, 0, 0);
  deserialize(s, "ab", 2); // length < 3 early return
  CHECK(s->pair_stack->len == 0, "cleared before early return");
  // truncated buffer: depth says 10, nothing stored, input ends
  char trunc[] = {0, 0, 0, 0, 0, 10, 3};
  deserialize(s, trunc, sizeof trunc);
  CHECK(s->pair_stack->len == 10, "truncated: depth restored, got %u",
        s->pair_stack->len);
  // corrupt: stored > depth
  char bad[] = {0, 0, 0, 0, 0, 1, 9};
  deserialize(s, bad, sizeof bad);
  CHECK(s->pair_stack->len == 1, "stored clamped to depth");
  tree_sitter_fey_external_scanner_destroy(s);
}

int main(void) {
  printf("table\n");
  test_table();
  printf("classify\n");
  test_classify();
  printf("hash collision\n");
  test_collision();
  printf("scan shapes\n");
  test_scan_shapes();
  printf("depth limit\n");
  test_depth_limit();
  printf("forgotten\n");
  test_forgotten_matches_any();
  printf("deserialize edges\n");
  test_deserialize_edge();
  printf("round trips\n");
  int depths[] = {0, 1, 50, 130, 200, 255};
  for (size_t k = 0; k < sizeof depths / sizeof depths[0]; k++) {
    round_trip(depths[k], false, 0, 0);
    round_trip(depths[k], true, 0, 0);
    round_trip(depths[k], true, 40, 60);
  }
  if (failures) {
    printf("%d FAILURE(S)\n", failures);
    return 1;
  }
  printf("all scanner tests passed\n");
  return 0;
}
