/**
 * @file A parser for the configurable Fey markup language
 * @author qaptoR <git@arxdaingneachd.org>
 * @license MIT
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

const asciiSymbols = [
  '"', "'", '`',
  '\\', '/', '|', '_',
  '#', '$', '%', '&', '@',
  ',', '.', ':', ';', '!', '?',
  '*', '+', '-', '=', '^', '~',
  '(', ')', '[', ']', '{', '}', '<', '>',
]

const tagTokens = [
  '@', '#', '$', '&', '%',
  '!', '?', '\\', '/',
  '-', '+', '*', '=', '~', '^',
  '.', ',', ':', ';',
]

const tagBrackets = [
  [ '[', ']' ],
  [ '{', '}' ],
  [ '(', ')' ],
  [ '<', '>' ],
]

const tagQuotes = [
  "'", '"', '`',
]

// const reTagName = /[a-zA-Z0-9_-]+[!?\\/\-+*=~^%@&#$]/;
// const reTagKey = /[a-zA-Z0-9_-]/;
const reTagName = /[a-zA-Z_][a-zA-Z0-9_-]*/;
const reTagKey = reTagName;
const reTagBracket = /[\[\]{}()<>]/;
const reTagToken =/[.,:;!?\\/\-+*=~^%@&#$]/
const reTagOpen = /[\[{(<][.,:;!?\\/\-_+*=~^%@&#$]/;
const reTagClose = /[.,:;!?\\/\-_+*=~^%@&#$][\]})>]/;

// Tags require a space/tab between their open/close markers and their content:
//   [# name #]    {$ name, value $}    ( name )#    *[ name ]  body *
const reWs = /[ \t]/;
const reWsPlus = /[ \t]+/;

const reCellEscape = /\\[|+*~\\]/;
const reSegment = /[.,:;!?\\/'"`\-+*=~^%@&#$\[\](){}<>]/;
const reFence = /[.,:;!?\\/'"`\-+*=~^%@&#$]+/;

export default grammar({
  name: "fey",

  extras: _ => [/[ \f\t\v\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000\ufeff]/],

  externals: $ => [
    $._standalone_tag_start,
    $._block_tag_start,
    $._block_tag_end,
    $._list_start,
    $._list_end,
    $._listitem_end,
    $._bullet,
    $._fence,
    $._signature,
    $._section_end,
    $._eof,  // Basically just '\0', but allows multiple to be matched
    $._tag_nl,
  ],


  inline: $ => [
    $._nl,
    $._eol,
    // $._ts_contents,
    // $._directive_list,
    // $._body_contents,
  ],

  conflicts: $ => [
    [$._tag_head, $._tag_multi_value_choice],
    // [ $._element ],
    // [ $.paragraph ],
  ],

  precedences: _ => [
    // ['document_directive', 'body_directive'],
    ['special', 'immediate', 'non-immediate'],
  ],

  rules: {
    document: $ => seq(
      optional(field('body', $.body)),
      repeat(field('subsection', $.section)),
    ),

    body: $ => $._body_contents,

    _body_contents: $ => choice(
      repeat1($._nl),

      seq(repeat($._nl), $._multis),

      seq(
        repeat($._nl),
        repeat1(seq(
          choice(
            seq($._multis, $._nl),

            seq(
              optional(choice(
                $.paragraph,
                // $.fndef,
              )),
              $._element,
            ),
          ),
          repeat($._nl),
        )),
        optional($._multis),
      ),
    ),

    _multis: $ => choice(
      $.paragraph,
      // $._directive_list,
      // $.fndef,
    ),

    _element: $ => choice(
      // $.comment,
      $.list,
      $.table,
      $.block,
      $.block_tag,
      $.standalone_simple_tag,
    ),

    section: $ => seq(
      field('heading', $.heading),
      // optional(field('metadata', $.metadata)),
      optional(field('body', $.body)),
      repeat(field('subsection', $.section)),
      $._section_end,
    ),

    heading: $ => seq(
      field('signature', $.signature),
      /[ \t]/,
      optional(field('title', $.title)),
      $._eol,
      // repeat($._nl),
    ),

    signature: $ => seq(
      $._signature,
      '  ',
      repeat1($.segment),
    ),

    segment: $ => seq(
      alias(/[a-zA-Z0-9_]*/, 'index'),
      alias(token.immediate(reSegment), 'delim'),
    ),

    title: $ => $._tagged_expr_multi_line,

    paragraph: $ => seq(
      // optional($._directive_list),
      // prec.left(1, $._multiline_tagged_text),
      $._multiline_tagged_text,
    ),

    list: $ => seq(
      // optional($._directive_list),
      $._list_start,  // captures indent length and bullet type
      repeat(seq($.listitem, $._listitem_end, repeat($._nl))),
      seq($.listitem, $._list_end)
    ),

    listitem: $ => seq(
      field('bullet', $.bullet),
      // $._two_spaces,
      /[ \t]{2,}/,
      // optional(field('checkbox', $.checkbox)),
      choice(
        $._eof,
        field('contents', $._body_contents),
      ),
    ),

    bullet: $ => seq(
      $._bullet,
      /[ \t]*/,
      $.segment,
    ),

    table: $ => prec.right(seq(
      // optional($._directive_list),
      field('crown', $.row),
      repeat(choice($.row, $.row_block, $.hr)),
      // repeat($.formula),
    )),

    row_block: $ => seq(
      field('cbo', $.cbo),
      repeat(choice($.row, $.cbi, $.hr)),
      field('cbe', $.cbe),
    ),

    row: $ => prec(1, seq(
      token(prec(1, '|')),
      repeat1(field('cell', $.cell)),
      $._eol,
    )),

    cell: $ => choice(seq(
      field('contents', alias($._tagged_cell_line, $.contents)),
      token(prec(1, '|')),
    ),
      alias(token(prec(1, /[ ]*\|/)), 'empty'),
    ),

    cbo: $ => seq(
      token(prec(1, 'v')),
      repeat1(field('cb_cell', $.cbo_cell)),
      $._eol,
    ),
    cbo_cell: $ => seq(
      prim_1(/[-]+/),
      choice(
        field('cb_corner', alias(prim_1(/[v]/), $.cb_corner)),
        prim_1(/[*]/),
      ),
    ),

    cbe: $ => seq(
      token(prec(1, '^')),
      repeat1(field('cb_cell', $.cbe_cell)),
      $._eol,
    ),
    cbe_cell: $ => seq(prim_1(/[-]+/), prim_1('^')),

    cbi: $ => seq(
      token(prec(1, '+')),
      repeat1(field('cb_cell', $.cbi_cell)),
      $._eol,
    ),
    cbi_cell: $ => choice(
      alias(seq(
        prim_1(/[~]+/),
        choice(
          field('cb_corner', alias(prim_1(/[+]/), $.cb_corner)),
          prim_1(/[*]/),
        )
      ), 'div'),
      field('cb_corner', alias(token(prec(1, /[ ]*[+]/)), $.vmerge)),
      seq(
        field('contents', alias($._tagged_cell_line, $.contents)),
        field('cb_corner', alias(token(prec(1, '+')), $.vmerge)),
      ),
    ),

    hr: $ => seq(
      token(prec(1, '+')),
      repeat1(seq(prim_1(/=+/), prim_1(/[+]/))),
      $._eol,
    ),

    // formula: $ => seq(
    //   caseInsensitive('#+tblfm:'),
    //   field('formula', optional($._expr_line)),
    //   $._eol,
    // ),

    block: $ => seq(
      // optional($._directive_list),
      $._fence,
      /[ \t]*/,
      field('openfence', $.fence),
      optional(seq(
        /[ \t]{2,}/, 
        field('name', $.expr),
        repeat(field('parameter', $.expr))
      )),
      $._nl,
      optional(field('contents', $.contents)),
      choice(seq(
        $._fence,
        /[ \t]*/,
        field('closefence', $.fence),
        $._eol,
      ), $._eof),
    ),

    fence: $ => reFence,

    contents: $ => seq(
      optional(/[ \t]+/),
      optional($._expr_line),
      repeat1($._nl),
      repeat(seq($._expr_line, repeat1($._nl))),
    ),

    // 76 thin alternatives (sigil x bracket). The expensive pieces (the head with
    // name/values, and the sigil-specific body) are shared rules; only the
    // start/close tokens vary per alternative.
    line_tag: $ => choice(
      ...tagTokens.flatMap((tok, ti) =>
        tagBrackets.map(([open, close]) => seq(
          field('tag_closure', alias(
            token(seq(sp_nim(tok), imm(open), imm(reWs))), $.tag_start)
          ),
          $._tag_head,
          tag_close($, ws_end(close), bare_end(close)),
          $[`_line_tag_tail_${ti}`],
        )),
      )
    ),

    // Shared by line_tag and simple_multi_tag.
    _tag_head: $ => seq(
      repeat($._tag_nl),
      field('name', alias(nim(reTagName), $.tag_name)),
      repeat($._tag_multi_value_choice),
      optional(alias(choice(nim(','), nim(';')), 'tag_delimiter')),
    ),

    // one per sigil: body and terminator depend only on the sigil
    ...Object.fromEntries(tagTokens.flatMap((tok, ti) => [
      [`_line_tag_body_${ti}`, $ => line_tag_body(tok, $)],
      [`_line_tag_tail_${ti}`, $ => seq(
        optional(seq(/[ \t]{2,}/, alias($[`_line_tag_body_${ti}`], $.body))),
        choice(
          $._eol,
          field('tag_closure', alias(nim(tok), $.body_end)),
        ),
      )],
    ])),

    block_tag: $ => seq(
      $._block_tag_start,
      /[ \t]*/,
      $._block_tag_open,
      choice(
        $._eol,
        seq($._nl, field('body', $.body)),
        seq(/[ \t]{2,}/, field('body', $.body)),
      ),
      $._block_tag_end,
    ),

    _block_tag_open: $ => choice(
      ...tagBrackets.map(([open, close]) => seq(
        field('tag_closure', alias(token.immediate(prec('special', seq(open, reWs))), $.tag_start)),
        repeat($._tag_nl),
        field('name', alias(nim(reTagName), $.tag_name)),
        repeat($._tag_multi_value_choice),
        optional(alias(choice(nim(','), nim(';')), 'tag_delimiter')),
        tag_close(
          $,
          ws_end(close, reTagToken),
          bare_end(close, reTagToken),
        ),
      ))
    ),

    standalone_simple_tag: $ => seq(
      $._standalone_tag_start,
      alias($.simple_multi_tag, $.simple_tag),
      $._eol,
    ),

    simple_multi_tag: $ => choice(
      ...tagBrackets.flatMap(([open, close]) =>
        tagTokens.map(tok => seq(
          field('tag_closure', alias(token(seq(nim(open), imm(tok), imm(reWs))), $.tag_start)),
          $._tag_head,
          tag_close($, ws_end(tok, close), bare_end(tok, close)),
        ))
      )
    ),

    simple_line_tag: $ => choice(
      ...tagBrackets.flatMap(([open, close]) =>
        tagTokens.map(tok => seq(
          field('tag_closure', alias(token(seq(nim(open), imm(tok), imm(reWs))), $.tag_start)),
          $._tag_line_head,
          field('tag_closure', alias(ws_end(tok, close), $.tag_end))
        ))
      )
    ),

    // Single-line counterpart of _tag_head, shared by every simple_line_tag.
    _tag_line_head: $ => seq(
      field('name', alias(nim(reTagName), $.tag_name)),
      repeat($._tag_line_value_choice),
      optional(alias(choice(nim(','), nim(';')), 'tag_delimiter')),
    ),

    _tag_multi_value_choice: $ => choice(
      seq(
        alias(nim(','), 'tag_delimiter'),
        repeat($._tag_nl),
        field('value', alias($._tag_value, $.value))
      ),
      seq(
        alias(nim(';'), 'tag_delimiter'),
        repeat($._tag_nl),
        field('key_value', alias($._tag_key_value, $.value))
      ),
    ),

    _tag_line_value_choice: $ => choice(
      seq(
        alias(nim(','), 'tag_delimiter'),
        field('value', alias($._tag_value, $.value))
      ),
      seq(
        alias(nim(';'), 'tag_delimiter'),
        field('key_value', alias($._tag_key_value, $.value))
      ),
    ),

    _tag_key_value: $ => seq(
      field('key', alias(nim(reTagKey), $.key)),
      alias(nim(/[.:=]/), 'tag_delimiter'),
      field('value', alias($._tag_value, $.value)),
    ),

    // A value word may not START with a closing bracket: otherwise ` ]` ties
    // with the bracket-only closer of line_tag and the word wins the lexer tie.
    _tag_value: $ => repeat1(tag_word(',;', ']})>')),

    _expr_line: $ => repeat1($.expr),

    _tagged_cell_line: $ => repeat1(choice(
      alias($.cell_expr, $.expr), alias($.simple_line_tag, $.simple_tag)
    )),

    _tagged_expr_line: $ => repeat1(choice(
      $.expr,
      alias($.simple_line_tag, $.simple_tag),
    )),

    _tagged_expr_multi_line: $ => repeat1(choice(
      $.expr,
      $.line_tag,
      alias($.simple_multi_tag, $.simple_tag),
    )),

    _multiline_tagged_text: $ => repeat1(
      seq($._tagged_expr_multi_line, $._eol)
    ),

    expr: $ => seq(
      expr('non-immediate', token),
      repeat(expr('immediate', token.immediate))
    ),

    cell_expr: $ => seq(
      expr('non-immediate', token, '', true),
      repeat(expr('immediate', token.immediate, '', true)),
    ),


    _nl: _ => choice('\r\n', '\r', '\n'),
    _eol: $ => choice($._nl, $._eof),
  }
});

// Closing marker of a tag. Either whitespace followed by the closer on the same
// line (` #]`), or, for multi-line tags, one or more newlines (the external
// _tag_nl already consumes the indentation) followed by the bare closer.
function tag_close($, spaced, bare) {
  return choice(
    field('tag_closure', alias(spaced, $.tag_end)),
    seq(
      repeat1($._tag_nl),
      field('tag_closure', alias(bare, $.tag_end)),
    ),
  );
}

// Closer token for the line after a _tag_nl, which has already consumed the
// newline and indentation. It is a separate token from ordinary value
// symbols (like `]`) so the parser can tell "closer" from "value".
function bare_end(...parts) {
  return token.immediate(prec('special', parts.length === 1 ? parts[0] : seq(...parts)));
}

// Closer token that must be preceded by at least one space/tab.
function ws_end(...parts) {
  return token.immediate(prec('special', seq(reWsPlus, ...parts)));
}

function line_tag_body(skip, $) {
  return repeat1(choice(
    tag_word(skip), $.line_tag,
    alias($.simple_multi_tag, $.simple_tag),
  ));
}

function tag_word(skip, noStart = '') {
  return seq(
    tag_expr('non-immediate', token, skip, noStart),
    repeat(tag_expr('immediate', token.immediate, skip)),
  );
}

function tag_expr(pr, tfunc, skip = '', noStart = '') {
  const esc = c => c.replace(/[\\\]\[^-]/g, '\\$&');
  const chars = skip.split('');
  // chars that may not be matched at all here (skip) or as a leading token (noStart)
  const excl = (skip + noStart).split('');
  const sym = new RegExp(`[^\\p{Z}\\p{L}\\p{N}\\t\\n\\r${excl.map(esc).join('')}]`);

  // escapes for any quote chars in skip, plus \\ 
  // const quotes = chars.filter(c => tagQuotes.includes(c));
  // const escapes = quotes.length
  const escapes = chars.length
    ? [alias(tfunc(prec(pr, new RegExp(`\\\\[${chars.map(esc).join('')}\\\\]`))), 'escape')]
    : [];

  return choice(
    ...escapes,
    ...asciiSymbols.filter(c => !excl.includes(c)).map(c => tfunc(prec(pr, c))),
    alias(tfunc(prec(pr, /\p{L}+/)), 'str'),
    alias(tfunc(prec(pr, /\p{N}+/)), 'num'),
    alias(tfunc(prec(pr, sym)), 'sym'),
  );
}

function expr(pr, tfunc, skip = '', cell = false) {
  skip = skip.split("")
  return choice(
    ...(cell ? [alias(tfunc(prec(pr, reCellEscape)), 'escape')] : []),
    ...asciiSymbols.filter(c => !skip.includes(c)).map(c => tfunc(prec(pr, c))),
    alias(tfunc(prec(pr, /\p{L}+/)), 'str'),
    alias(tfunc(prec(pr, /\p{N}+/)), 'num'),
    alias(tfunc(prec(pr, /[^\p{Z}\p{L}\p{N}\t\n\r]/)), 'sym'),
  )
}

function sp_imm(item) { return token(prec.left('special', item)); }
function sp_nim(item) { return token.immediate(prec('special', item)); }
function nim(item) { return token(prec('non-immediate', item)); }
function imm(item) { return token.immediate(prec('immediate', item)); }
function prim_1(item) {
  return token.immediate(prec(1, item));
}
