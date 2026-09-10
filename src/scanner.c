// scanner.c
#include "tree_sitter/parser.h"
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

enum TokenType { LINE_CONTENT, SECTION_START, SECTION_END };

#define MAX_DEPTH_STACK 100

typedef struct {
  int depths[MAX_DEPTH_STACK];
  int size;
} Scanner;

void *tree_sitter_fey_external_scanner_create() {
  return calloc(1, sizeof(Scanner));
}

void tree_sitter_fey_external_scanner_destroy(void *payload) { free(payload); }

unsigned tree_sitter_fey_external_scanner_serialize(void *payload,
                                                    char *buffer) {
  Scanner *scanner = (Scanner *)payload;
  unsigned size = scanner->size * sizeof(int);
  if (size > TREE_SITTER_SERIALIZATION_BUFFER_SIZE)
    return 0;
  memcpy(buffer, scanner->depths, size);
  return size;
}

void tree_sitter_fey_external_scanner_deserialize(void *payload,
                                                  const char *buffer,
                                                  unsigned length) {
  Scanner *scanner = (Scanner *)payload;
  scanner->size = length / sizeof(int);
  if (scanner->size > 0) {
    memcpy(scanner->depths, buffer, length);
  }
}

static int calculate_heading_depth(TSLexer *lexer) {
  if (lexer->lookahead != ' ')
    return -1;
  lexer->advance(lexer, false);
  if (lexer->lookahead != ' ')
    return -1;
  lexer->advance(lexer, false);

  int depth = 0;
  bool in_prefix = false;

  while (true) {
    while (iswalnum(lexer->lookahead) || lexer->lookahead == '_' ||
           lexer->lookahead == '<' || lexer->lookahead == '>' ||
           lexer->lookahead == '{' || lexer->lookahead == '}' ||
           lexer->lookahead == '(' || lexer->lookahead == ')' ||
           lexer->lookahead == '[' || lexer->lookahead == ']') {
      lexer->advance(lexer, false);
    }

    if (lexer->lookahead == '.' || lexer->lookahead == ':' ||
        lexer->lookahead == ';' || lexer->lookahead == '/' ||
        lexer->lookahead == '\\' || lexer->lookahead == '!' ||
        lexer->lookahead == '\'' || lexer->lookahead == '"' ||
        lexer->lookahead == '-' || lexer->lookahead == '+' ||
        lexer->lookahead == '*' || lexer->lookahead == '=' ||
        lexer->lookahead == '@' || lexer->lookahead == '&' ||
        lexer->lookahead == '#' || lexer->lookahead == '$' ||
        lexer->lookahead == '%' || lexer->lookahead == '?' ||
        lexer->lookahead == ',') {
      depth++;
      in_prefix = true;
      lexer->advance(lexer, false);
    } else {
      break;
    }
  }

  return in_prefix ? depth : -1;
}

bool tree_sitter_fey_external_scanner_scan(void *payload, TSLexer *lexer,
                                           const bool *valid_symbols) {
  Scanner *scanner = (Scanner *)payload;

  // Pop remaining sections if we hit the end of the file
  if (lexer->eof(lexer)) {
    if (valid_symbols[SECTION_END] && scanner->size > 0) {
      scanner->size--;
      lexer->result_symbol = SECTION_END;
      return true;
    }
    return false;
  }

  bool is_empty_line = (lexer->lookahead == 0 || lexer->lookahead == '\n' ||
                        lexer->lookahead == '\r');

  // Mark end immediately so SECTION_START and SECTION_END are generated as
  // zero-width tokens
  lexer->mark_end(lexer);

  int heading_depth = -1;
  if (lexer->get_column(lexer) == 0 && !is_empty_line) {
    heading_depth = calculate_heading_depth(lexer);
  }

  if (heading_depth > 0) {
    int current_depth =
        scanner->size > 0 ? scanner->depths[scanner->size - 1] : 0;

    // Emits SECTION_END without consuming chars if the upcoming heading breaks
    // out of the current depth
    if (valid_symbols[SECTION_END] && heading_depth <= current_depth &&
        scanner->size > 0) {
      scanner->size--;
      lexer->result_symbol = SECTION_END;
      return true;
    }

    // Emits SECTION_START without consuming chars to push into a nested
    // subsection
    if (valid_symbols[SECTION_START] && heading_depth > current_depth) {
      if (scanner->size < MAX_DEPTH_STACK) {
        scanner->depths[scanner->size++] = heading_depth;
      }
      lexer->result_symbol = SECTION_START;
      return true;
    }

    // Reject LINE_CONTENT and fallback so Tree-sitter's internal lexer parses
    // the actual heading node
    return false;
  }

  if (valid_symbols[LINE_CONTENT]) {
    if (is_empty_line) {
      return false;
    }

    while (lexer->lookahead != 0 && lexer->lookahead != '\n' &&
           lexer->lookahead != '\r') {
      lexer->advance(lexer, false);
    }

    lexer->mark_end(lexer);
    lexer->result_symbol = LINE_CONTENT;
    return true;
  }

  return false;
}
