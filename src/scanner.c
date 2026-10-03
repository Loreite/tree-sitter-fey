// scanner.c
#include "tree_sitter/parser.h"
#include <assert.h>
#include <stdio.h>
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

enum TokenType {
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
};

typedef enum {
  NOTABULLET,
  ISABULLET,
} Bullet;

typedef struct {
  uint32_t len;
  uint32_t cap;
  int16_t *data;
} stack;

typedef struct {
  stack *indent_length_stack;
  stack *bullet_stack;
  stack *section_stack;

  // stack *tag_bracket_stack;
  // stack *tag_token_stack;
  stack *tag_indent_length_stack;

  stack *fence_indent_stack;
  stack *fence_width_stack;
  stack *fence_char_stack;

  bool is_at_section_start;
  int16_t base_indent;
} Scanner;

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }
static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

// Serialized layout (must match deserialize exactly):
//   [base_indent hi][base_indent lo]
//   [fence_len (0|1)] [fence indent][fence width][fence char]   (if fence_len)
//   [tag_count]    [tag indents ...]        (all entries; no sentinel)
//   [indent_count] [indents ...][bullets ...]  (entries after the sentinel)
//   [sections ...]                          (entries after the sentinel; rest)
unsigned serialize(Scanner *scanner, char *buffer) {
  size_t i = 0;

  PUT((scanner->base_indent >> 8) & 0xFF);
  PUT(scanner->base_indent & 0xFF);

  // fences don't nest: at most one entry is ever live
  if (scanner->fence_indent_stack->len > 0) {
    PUT(1);
    PUT(VEC_BACK(scanner->fence_indent_stack));
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
void deserialize(Scanner *scanner, const char *buffer, unsigned length) {
  VEC_CLEAR(scanner->section_stack);
  VEC_PUSH(scanner->section_stack, 0);
  VEC_CLEAR(scanner->indent_length_stack);
  VEC_PUSH(scanner->indent_length_stack, -1);
  VEC_CLEAR(scanner->bullet_stack);
  VEC_PUSH(scanner->bullet_stack, NOTABULLET);

  VEC_CLEAR(scanner->fence_indent_stack);
  VEC_CLEAR(scanner->fence_width_stack);
  VEC_CLEAR(scanner->fence_char_stack);
  VEC_CLEAR(scanner->tag_indent_length_stack);

  scanner->base_indent = -1;

  if (length < 3)
    return;

  size_t i = 0;

  uint8_t base_hi = GET(); // separate statements: operand order is unspecified
  uint8_t base_lo = GET();
  scanner->base_indent = (int16_t)((base_hi << 8) | base_lo);

  uint8_t fence_len = GET();
  if (fence_len > 0) {
    VEC_PUSH(scanner->fence_indent_stack, GET());
    VEC_PUSH(scanner->fence_width_stack, GET());
    VEC_PUSH(scanner->fence_char_stack, GET());
  }

  uint8_t tag_count = GET();
  for (uint8_t j = 0; j < tag_count && i <= length; j++) {
    VEC_PUSH(scanner->tag_indent_length_stack, GET());
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
  // index 0 is the sentinel; never pop it (len is unsigned: 0 - 1 wraps)
  if (scanner->indent_length_stack->len > 1)
    VEC_POP(scanner->indent_length_stack);
  if (scanner->bullet_stack->len > 1)
    VEC_POP(scanner->bullet_stack);
  lexer->result_symbol = LIST_END;
  return true;
}

static bool dedent_block_tag(Scanner *scanner, TSLexer *lexer) {
  // VEC_POP(scanner->tag_bracket_stack);
  // VEC_POP(scanner->tag_token_stack);
  if (scanner->tag_indent_length_stack->len > 0)
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
  return ( //
      lexer->lookahead == '.' || lexer->lookahead == ',' ||
      lexer->lookahead == ':' || lexer->lookahead == ';' ||
      lexer->lookahead == '!' || lexer->lookahead == '?' ||
      lexer->lookahead == '/' || lexer->lookahead == '\\' ||
      lexer->lookahead == '-' || lexer->lookahead == '+' ||
      lexer->lookahead == '*' || lexer->lookahead == '^' ||
      lexer->lookahead == '%' || lexer->lookahead == '=' ||
      lexer->lookahead == '~' || lexer->lookahead == '@' ||
      lexer->lookahead == '&' || lexer->lookahead == '#' ||
      lexer->lookahead == '$'
      //
  );
}

static bool check_closure(TSLexer *lexer, bool open, bool close) {
  return (open && (lexer->lookahead == '[' || lexer->lookahead == '(' ||
                   lexer->lookahead == '{' || lexer->lookahead == '<')) ||
         (close && (lexer->lookahead == ']' || lexer->lookahead == ')' ||
                    lexer->lookahead == '}' || lexer->lookahead == '>'));
}

static bool check_segment(TSLexer *lexer) {
  while (check_token(lexer)) {
    skip(lexer);
  }
  return (check_delimiter(lexer) || check_closure(lexer, true, true));
}

static bool istabspace(TSLexer *lexer) {
  return (lexer->lookahead == ' ' || lexer->lookahead == '\t');
}

static bool indent_is_two_space_or_tab(int16_t indent_length) {
  return (indent_length == 2 || indent_length == 9 || indent_length == 16);
}

Bullet getbullet(TSLexer *lexer, bool bullet) {
  bool matched = false;
  if (check_delimiter(lexer) || check_closure(lexer, true, true)) {
    advance(lexer);
    matched = true;
  } else if (check_token(lexer)) {
    do {
      advance(lexer);
    } while (check_token(lexer));
    if (check_delimiter(lexer) || check_closure(lexer, true, true)) {
      advance(lexer);
      matched = true;
    }
  }
  if (!matched)
    return NOTABULLET;
  if (bullet)
    return ISABULLET;

  if (istabspace(lexer)) {
    skip(lexer);
  } else
    return NOTABULLET;

  if (istabspace(lexer)) {
    return ISABULLET;
  }

  return NOTABULLET;
}

bool scan(Scanner *scanner, TSLexer *lexer, const bool *valid_symbols) {
  if (in_error_recovery(valid_symbols))
    return false;

  scanner->is_at_section_start = false;

  // Handle explicit tag newlines with active list-indent checks
  if (valid_symbols[TAG_NL]) {
    while (istabspace(lexer))
      advance(lexer);
    if (lexer->lookahead == '\r') {
      advance(lexer);
    }
    if (lexer->lookahead == '\n') {
      advance(lexer);

      int16_t indent_length = 0;
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

      // Allow empty lines or lines with whitespace only inside multi-line tags
      if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
        lexer->result_symbol = TAG_NL;
        return true;
      }

      // Check if we are inside a list item
      if (scanner->indent_length_stack->len > 1) {
        if (indent_length < VEC_BACK(scanner->indent_length_stack)) {
          return false;
        }
      } else if (scanner->tag_indent_length_stack->len > 1) {
        if (indent_length < VEC_BACK(scanner->tag_indent_length_stack)) {
          return false;
        }
      }

      lexer->result_symbol = TAG_NL;
      return true;
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
      //
      if (valid_symbols[LIST_END]) {
        lexer->result_symbol = LIST_END;
        //
      } else if (valid_symbols[BLOCK_TAG_END]) {
        lexer->result_symbol = BLOCK_TAG_END;
        //
      } else if (valid_symbols[SECTION_END]) {
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
  }

  // - Listitem ends
  int16_t newlines = 0;
  if (valid_symbols[BLOCK_TAG_START] && check_closure(lexer, true, false)) {
    skip(lexer);
    if (check_tag_token(lexer))
      return false;
    return indent_block_tag(scanner, lexer, indent_length);
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
        if (valid_symbols[BLOCK_TAG_END] &&
            scanner->tag_indent_length_stack->len > 0) {
          return dedent_block_tag(scanner, lexer);
        }
        if (valid_symbols[LIST_END]) {
          return dedent(scanner, lexer);
        }
        break; // Escape if neither applies
      } else if (lexer->lookahead == '\n') {
        if (++newlines > 1) {
          if (valid_symbols[BLOCK_TAG_END] &&
              scanner->tag_indent_length_stack->len > 0) {
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

    if (valid_symbols[BLOCK_TAG_END] &&
        scanner->tag_indent_length_stack->len > 0) {
      if (indent_length <= VEC_BACK(scanner->tag_indent_length_stack)) {
        return dedent_block_tag(scanner, lexer);
      }
    }

    if (indent_length < VEC_BACK(scanner->indent_length_stack)) {
      return dedent(scanner, lexer);
    } else if (indent_length == VEC_BACK(scanner->indent_length_stack)) {
      if (getbullet(lexer, false) == VEC_BACK(scanner->bullet_stack)) {
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
  int32_t fence_char = lexer->lookahead;
  int16_t fence_width = 0;
  bool fenceable = true;
  bool segmentable = true;

  while (check_token(lexer) || check_delimiter(lexer) ||
         check_closure(lexer, true, true) //
  ) {
    if (check_token(lexer)) {
      while (check_token(lexer))
        skip(lexer);
      segmentable = false;
      fenceable = false;
    } else if (check_delimiter(lexer)) {
      segments += 1;
      segmentable = true;
      if (fenceable && lexer->lookahead == fence_char) {
        fence_width += 1;
      } else {
        fenceable = false;
      }
      skip(lexer);
    } else if (check_closure(lexer, true, true)) {
      segments += 1;
      segmentable = true;
      fenceable = false;
      skip(lexer);
    }
  }

  bool fence_looked_ahead = false;
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

    if (valid_symbols[FENCE] && fence_width >= 3) {
      if (scanner->fence_indent_stack->len == 0) {
        if (valid_fence_suffix) {
          VEC_PUSH(scanner->fence_indent_stack, indent_length);
          VEC_PUSH(scanner->fence_width_stack, fence_width);
          VEC_PUSH(scanner->fence_char_stack, fence_char);
          lexer->result_symbol = FENCE;
          return true;
        }
      } else if (VEC_BACK(scanner->fence_indent_stack) == indent_length &&
                 VEC_BACK(scanner->fence_width_stack) == fence_width &&
                 VEC_BACK(scanner->fence_char_stack) == fence_char) {
        VEC_POP(scanner->fence_indent_stack);
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

  if (lexer->lookahead != '\n' && lexer->lookahead != '\r') {
    bool has_second_space = false;
    if (fence_looked_ahead || istabspace(lexer)) {
      if (!fence_looked_ahead)
        skip(lexer);
      if (istabspace(lexer)) {
        has_second_space = true;
      }
    }

    is_bullet = (segments == 1 && has_second_space);
    is_signature = (segments > 0 && !has_second_space);
  }

  if (is_bullet && newlines == 0) {
    if (valid_symbols[BULLET]                                      //
        && indent_length == VEC_BACK(scanner->indent_length_stack) //
    ) {
      lexer->result_symbol = BULLET;
      return true;

    } else if (valid_symbols[LIST_START]                                 //
               && indent_length > VEC_BACK(scanner->indent_length_stack) //
    ) {
      return indent(scanner, lexer, indent_length, ISABULLET);
    }
    return false;
  }

  if (is_signature) {
    if (scanner->base_indent == -1) {
      scanner->base_indent = indent_length >= 2 ? indent_length - 2 : 0;
    }

    if (indent_length == scanner->base_indent + 2) {
      if (scanner->is_at_section_start && valid_symbols[LIST_END]) {
        return dedent(scanner, lexer);
      } else if (scanner->is_at_section_start && valid_symbols[BLOCK_TAG_END]) {
        return dedent_block_tag(scanner, lexer);
      }

      if (valid_symbols[SECTION_END]                      //
          && segments <= VEC_BACK(scanner->section_stack) //
      ) {
        VEC_POP(scanner->section_stack);
        lexer->result_symbol = SECTION_END;
        return true;

      } else if (valid_symbols[SIGNATURE]) {
        VEC_PUSH(scanner->section_stack, segments);
        lexer->result_symbol = SIGNATURE;
        return true;
      }
    }
  }

  return false;
}

void *tree_sitter_fey_external_scanner_create() {
  Scanner *scanner = (Scanner *)calloc(1, sizeof(Scanner));
  scanner->indent_length_stack = (stack *)calloc(1, sizeof(stack));
  scanner->bullet_stack = (stack *)calloc(1, sizeof(stack));
  scanner->section_stack = (stack *)calloc(1, sizeof(stack));

  // scanner->tag_bracket_stack = (stack *)calloc(1, sizeof(stack));
  // scanner->tag_token_stack = (stack *)calloc(1, sizeof(stack));
  scanner->tag_indent_length_stack = (stack *)calloc(1, sizeof(stack));

  scanner->fence_indent_stack = (stack *)calloc(1, sizeof(stack));
  scanner->fence_width_stack = (stack *)calloc(1, sizeof(stack));
  scanner->fence_char_stack = (stack *)calloc(1, sizeof(stack));

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

  // VEC_FREE(scanner->tag_bracket_stack);
  // VEC_FREE(scanner->tag_token_stack);
  VEC_FREE(scanner->tag_indent_length_stack);

  VEC_FREE(scanner->fence_indent_stack);
  VEC_FREE(scanner->fence_width_stack);
  VEC_FREE(scanner->fence_char_stack);

  free(scanner->fence_indent_stack);
  free(scanner->fence_width_stack);
  free(scanner->fence_char_stack);

  // free(scanner->tag_bracket_stack);
  // free(scanner->tag_token_stack);
  free(scanner->tag_indent_length_stack);

  free(scanner->indent_length_stack);
  free(scanner->bullet_stack);
  free(scanner->section_stack);
  free(scanner);
}
