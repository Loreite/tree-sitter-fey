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

const reCellEscape = /\\[|+*~\\]/;
const esc = c => c.replace(/[\\\]\[^-]/g, '\\$&');

const reSegment = /[.,:;!?\\/'"`\-+*=~^%@&#$\[\](){}<>]/;
const reFence = /[.,:;!?\\/'"`\-+*=~^%@&#$]+/;

export default grammar({
  name: "fey",

  extras: _ => [/[ \f\t\v\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000\ufeff]/],

  externals: $ => [
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
    $._body_contents,
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
      // $.tag,
      $.table,
      $.block
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

    simple_multi_tag: $ => choice(
      ...tagBrackets.flatMap(([open, close]) =>
        tagTokens.map(tok => seq(
          field('tag_closure', alias(token(seq(nim(open), imm(tok))), $.tag_start)),

          repeat($._tag_nl),
          field('name', alias(nim(reTagName), $.tag_name)),
          repeat($._tag_multi_value_choice),
          optional(choice(nim(','), nim(';'))),
          repeat($._tag_nl),

          field('tag_closure', alias(token(seq(nim(tok), imm(close))), $.tag_end))
        ))
      )
    ),

    simple_line_tag: $ => choice(
      ...tagBrackets.flatMap(([open, close]) =>
        tagTokens.map(tok => seq(
          field('tag_closure', alias(token(seq(nim(open), imm(tok))), $.tag_start)),

          field('name', alias(nim(reTagName), $.tag_name)),
          repeat($._tag_line_value_choice),
          optional(choice(nim(','), nim(';'))),

          field('tag_closure', alias(token(seq(nim(tok), imm(close))), $.tag_end))
        ))
      )
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

    _tag_value: $ => choice(
      // alias(token(tag_word(',;' + tagQuotes.join(''))), 'bare'),
      tag_word(',;' + tagQuotes.join('')),

      ...tagQuotes.map(q => seq(
        nim(q),
        repeat(tag_word(q)),
        nim(q),
      )),
      // ...tagQuotes.map(q => alias(token(seq(
      //   nim(q),
      //   repeat(tag_word(q)),
      //   nim(q),
      // )), 'quoted')),
    ),

    _expr_line: $ => repeat1($.expr),

    _tagged_cell_line: $ => repeat1(choice(
      alias($.cell_expr, $.expr), alias($.simple_line_tag, $.simple_tag)
    )),

    _tagged_expr_line: $ => repeat1(choice(
      $.expr, alias($.simple_line_tag, $.simple_tag)
    )),

    _tagged_expr_multi_line: $ => repeat1(choice(
      $.expr, alias($.simple_multi_tag, $.simple_tag)
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


function tag_word(skip) {
  return seq(
    tag_expr('non-immediate', token, skip),
    repeat(tag_expr('immediate', token.immediate, skip)),
  );
}

function tag_expr(pr, tfunc, skip = '') {
  const chars = skip.split('');
  const sym = new RegExp(`[^\\p{Z}\\p{L}\\p{N}\\t\\n\\r${chars.map(esc).join('')}]`);

  // escapes for any quote chars in skip, plus \\ 
  const quotes = chars.filter(c => tagQuotes.includes(c));
  const escapes = quotes.length
    ? [alias(tfunc(prec(pr, new RegExp(`\\\\[${quotes.join('')}\\\\]`))), 'escape')]
    : [];

  return choice(
    ...escapes,
    ...asciiSymbols.filter(c => !chars.includes(c)).map(c => tfunc(prec(pr, c))),
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
