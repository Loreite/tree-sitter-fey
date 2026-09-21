// scanner.c
#include "tree_sitter/parser.h"
#include <assert.h>
#include <stdio.h>
#include <strings.h>
#include <wctype.h>
// #include <stdlib.h>
// #include <string.h>

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

enum TokenType {
  LIST_START,
  LIST_END,
  LISTITEM_END,
  BULLET,
  FENCE,
  SIGNATURE,
  SECTION_END,
  ENDOFFILE,
};

typedef enum {
  NOTABULLET,
  ISABULLET,
  // DASH,
  // PLUS,
  // STAR,
  // LOWERDOT,
  // UPPERDOT,
  // LOWERPAREN,
  // UPPERPAREN,
  // NUMDOT,
  // NUMPAREN,
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

  stack *fence_indent_stack;
  stack *fence_width_stack;
  stack *fence_char_stack;
} Scanner;

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }
static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

unsigned serialize(Scanner *scanner, char *buffer) {
  size_t i = 0;

  buffer[i++] = scanner->fence_indent_stack->len;
  if (scanner->fence_indent_stack->len > 0) {
    buffer[i++] = scanner->fence_indent_stack->data[0];
    buffer[i++] = scanner->fence_width_stack->data[0];
    buffer[i++] = scanner->fence_char_stack->data[0];
  }

  size_t indent_count = scanner->indent_length_stack->len - 1;
  if (indent_count > UINT8_MAX)
    indent_count = UINT8_MAX;
  buffer[i++] = indent_count;

  int iter = 1;
  for (; iter < scanner->indent_length_stack->len &&
         i < TREE_SITTER_SERIALIZATION_BUFFER_SIZE;
       ++iter) {
    buffer[i++] = scanner->indent_length_stack->data[iter];
  }

  iter = 1;
  for (; iter < scanner->bullet_stack->len &&
         i < TREE_SITTER_SERIALIZATION_BUFFER_SIZE;
       ++iter) {
    buffer[i++] = scanner->bullet_stack->data[iter];
  }

  iter = 1;
  for (; iter < scanner->section_stack->len &&
         i < TREE_SITTER_SERIALIZATION_BUFFER_SIZE;
       ++iter) {
    buffer[i++] = scanner->section_stack->data[iter];
  }

  return i;
}

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

  if (length == 0)
    return;

  size_t i = 0;

  uint8_t fence_len = (uint8_t)buffer[i++];
  if (fence_len > 0) {
    VEC_PUSH(scanner->fence_indent_stack, buffer[i++]);
    VEC_PUSH(scanner->fence_width_stack, buffer[i++]);
    VEC_PUSH(scanner->fence_char_stack, buffer[i++]);
  }

  size_t indent_count = (uint8_t)buffer[i++];

  // Use an independent loop counter (j) so 'i' can safely track the buffer
  // index
  for (size_t j = 0; j < indent_count; j++) {
    VEC_PUSH(scanner->indent_length_stack, buffer[i++]);
  }

  for (size_t j = 0; j < indent_count; j++) {
    VEC_PUSH(scanner->bullet_stack, buffer[i++]);
  }

  // Safely consume all remaining bytes for the section stack
  while (i < length) {
    VEC_PUSH(scanner->section_stack, buffer[i++]);
  }
}
// void deserialize(Scanner *scanner, const char *buffer, unsigned length) {
//   VEC_CLEAR(scanner->section_stack);
//   VEC_PUSH(scanner->section_stack, 0);
//   VEC_CLEAR(scanner->indent_length_stack);
//   VEC_PUSH(scanner->indent_length_stack, -1);
//   VEC_CLEAR(scanner->bullet_stack);
//   VEC_PUSH(scanner->bullet_stack, NOTABULLET);
//
//   VEC_CLEAR(scanner->fence_indent_stack);
//   VEC_CLEAR(scanner->fence_width_stack);
//   VEC_CLEAR(scanner->fence_char_stack);
//
//   if (length == 0)
//     return;
//
//   size_t i = 0;
//
//   uint8_t fence_len = (uint8_t)buffer[i++];
//   if (fence_len > 0) {
//     VEC_PUSH(scanner->fence_indent_stack, buffer[i++]);
//     VEC_PUSH(scanner->fence_width_stack, buffer[i++]);
//     VEC_PUSH(scanner->fence_char_stack, buffer[i++]);
//   }
//
//   size_t indent_count = (uint8_t)buffer[i++];
//   for (; i <= indent_count; i++)
//     VEC_PUSH(scanner->indent_length_stack, buffer[i]);
//   for (; i <= 2 * indent_count; i++)
//     VEC_PUSH(scanner->bullet_stack, buffer[i]);
//   for (; i < length; i++)
//     VEC_PUSH(scanner->section_stack, buffer[i]);
// }

static bool in_error_recovery(const bool *valid_symbols) {
  return (valid_symbols[LIST_START]                                 //
          && valid_symbols[LIST_END]                                //
          && valid_symbols[LISTITEM_END]                            //
          && valid_symbols[BULLET]                                  //
          && valid_symbols[FENCE]                                   //
          && valid_symbols[SIGNATURE]                               //
          && valid_symbols[SECTION_END] && valid_symbols[ENDOFFILE] //
  );
}

static bool dedent(Scanner *scanner, TSLexer *lexer) {
  VEC_POP(scanner->indent_length_stack);
  VEC_POP(scanner->bullet_stack);
  lexer->result_symbol = LIST_END;
  return true;
}
static bool indent(                                                        //
    Scanner *scanner, TSLexer *lexer, int16_t indent_length, Bullet bullet //
) {
  VEC_PUSH(scanner->indent_length_stack, indent_length);
  VEC_PUSH(scanner->bullet_stack, bullet);
  lexer->result_symbol = LIST_START;
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

static bool check_closure(TSLexer *lexer, bool open, bool close) {
  return (open && (lexer->lookahead == '[' || lexer->lookahead == '(' ||
                   lexer->lookahead == '{' || lexer->lookahead == '<')) //
         || (close && (lexer->lookahead == ']' || lexer->lookahead == ')' ||
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
  return (indent_length == 2    //
          || indent_length == 9 //
          || indent_length == 16);
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

  // - Section ends
  int16_t indent_length = 0;
  lexer->mark_end(lexer);
  while (true) {
    if (lexer->lookahead == ' ') {
      indent_length++;
    } else if (lexer->lookahead == '\t') {
      indent_length += 8;
    } else if (lexer->lookahead == '\0') {
      if (valid_symbols[LIST_END]) {
        lexer->result_symbol = LIST_END;
      } else if (valid_symbols[SECTION_END]) {
        lexer->result_symbol = SECTION_END;
      } else if (valid_symbols[ENDOFFILE]) {
        lexer->result_symbol = ENDOFFILE;
      } else
        return false;

      return true;
    } else {
      break;
    }
    skip(lexer);
  }

  // - Listiem ends
  // Listend -> end of a line, looking for:
  // 1. dedent
  // 2. same indent, not a bullet
  // 3. two eols
  int16_t newlines = 0;
  if (valid_symbols[LIST_END] || valid_symbols[LISTITEM_END]) {
    while (true) {
      if (lexer->lookahead == ' ') {
        indent_length++;
      } else if (lexer->lookahead == '\t') {
        indent_length += 8;
      } else if (lexer->lookahead == '\0') {
        return dedent(scanner, lexer);
      } else if (lexer->lookahead == '\n') {
        if (++newlines > 1)
          return dedent(scanner, lexer);
        indent_length = 0;
      } else {
        break;
      }
      skip(lexer);
    }

    if (indent_length < VEC_BACK(scanner->indent_length_stack)) {
      return dedent(scanner, lexer);
    } else if (indent_length == VEC_BACK(scanner->indent_length_stack)) {
      if (getbullet(lexer, false) == VEC_BACK(scanner->bullet_stack)) {
        lexer->result_symbol = LISTITEM_END;
        return true;
      }

      return dedent(scanner, lexer);
    }
  }

  // Zero-width lookahead for listitem bullets and heading signatures
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

  // start/end of raw Block
  if (fenceable && valid_symbols[FENCE] && indent_length != 2     //
      && (iswspace(lexer->lookahead) || lexer->lookahead == '\0') //
      && fence_width >= 3                                         //
  ) {

    bool has_active_fence = scanner->fence_indent_stack->len > 0;
    if (!has_active_fence) {
      // Push State
      VEC_PUSH(scanner->fence_indent_stack, indent_length);
      VEC_PUSH(scanner->fence_width_stack, fence_width);
      VEC_PUSH(scanner->fence_char_stack, fence_char);

      lexer->result_symbol = FENCE;
      return true;
    } else {
      // Validate identical parameters against the max-size-1 stack
      if (VEC_BACK(scanner->fence_indent_stack) == indent_length &&
          VEC_BACK(scanner->fence_width_stack) == fence_width &&
          VEC_BACK(scanner->fence_char_stack) == fence_char //
      ) {
        // Pop State
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
    if (istabspace(lexer)) {
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

  if (indent_length == 2 && is_signature) {
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

  return false;
}

void *tree_sitter_fey_external_scanner_create() {
  Scanner *scanner = (Scanner *)calloc(1, sizeof(Scanner));
  scanner->indent_length_stack = (stack *)calloc(1, sizeof(stack));
  scanner->bullet_stack = (stack *)calloc(1, sizeof(stack));
  scanner->section_stack = (stack *)calloc(1, sizeof(stack));

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

  VEC_FREE(scanner->fence_indent_stack);
  VEC_FREE(scanner->fence_width_stack);
  VEC_FREE(scanner->fence_char_stack);

  free(scanner->fence_indent_stack);
  free(scanner->fence_width_stack);
  free(scanner->fence_char_stack);

  free(scanner->indent_length_stack);
  free(scanner->bullet_stack);
  free(scanner->section_stack);
  free(scanner);
}
