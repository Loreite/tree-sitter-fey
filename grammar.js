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
    $._tag_start,
    $._tag_end,
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
      field('contents', alias($._tagged_expr_line, $.contents)),
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
        field('contents', alias($._tagged_expr_line, $.contents)),
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

    // simple_multi_tag: $ => prec.dynamic(1, choice(
    simple_multi_tag: $ => choice(
      seq(
        $._tag_start,
        alias(nim(reTagOpen), 'tag_start'),
        repeat($._nl),
        field('name', alias(nim(reTagName), 'tag_name')),
        repeat($._nl),
        repeat(seq( $.tag_value, repeat($._nl))),
        $._tag_end,
        alias(nim(reTagClose), 'tag_end'),
      ),
    ),
    // )),

    // simple_line_tag: $ => prec.dynamic(1, seq(
    simple_line_tag: $ => seq(
      $._tag_start,
      alias(nim(reTagOpen), 'tag_start'),
      field('name', alias(nim(reTagName), 'tag_name')),
      repeat($.tag_value),
      $._tag_end,
      alias(nim(reTagClose), 'tag_end'),
    ),
    // )),

    tag_value: $ => choice(
      seq(
        alias(nim(','), 'value_delimiter'),
        field('arg_val', alias($._tag_value, 'value'))
      ),
      seq(
        alias(nim(';'), 'value_delimiter'),
        field('arg_keyval', alias(seq(
          alias(nim(reTagKey), 'key'),
          alias(nim(/[.:=]/), 'tag_delimiter'),
          alias($._tag_value, 'value'),
        ), 'keyval'))
      ),
    ),

    _tag_value: $ => choice(
      // tag_word(',;.:=' + tagQuotes.join('')),
      tag_word(',;' + tagQuotes.join('')),

      ...tagQuotes.map(q => seq(
        nim(q),
        repeat(tag_word(q)),
        nim(q),
      )),
    ),

    _expr_line: $ => repeat1($.expr),
    _tagged_expr_line: $ => repeat1(choice($.expr, $.simple_line_tag)),
    _tagged_expr_multi_line: $ => repeat1(choice($.expr, $.simple_multi_tag)),

    _multiline_tagged_text: $ => repeat1(
      seq($._tagged_expr_multi_line, $._eol)
    ),

    expr: $ => seq(
      expr('non-immediate', token),
      repeat(expr('immediate', token.immediate))
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
  const esc = c => c.replace(/[\\\]\[^-]/g, '\\$&');
  const chars = skip.split('');
  const sym = new RegExp(`[^\\p{Z}\\p{L}\\p{N}\\t\\n\\r${chars.map(esc).join('')}]`);

  return choice(
    ...asciiSymbols.filter(c => !chars.includes(c)).map(c => tfunc(prec(pr, c))),
    alias(tfunc(prec(pr, /\p{L}+/)), 'str'),
    alias(tfunc(prec(pr, /\p{N}+/)), 'num'),
    alias(tfunc(prec(pr, sym)), 'sym'),
  );
}

function expr(pr, tfunc, skip = '') {
  skip = skip.split("")
  return choice(
    ...asciiSymbols.filter(c => !skip.includes(c)).map(c => tfunc(prec(pr, c))),
    alias(tfunc(prec(pr, /\p{L}+/)), 'str'),
    alias(tfunc(prec(pr, /\p{N}+/)), 'num'),
    alias(tfunc(prec(pr, /[^\p{Z}\p{L}\p{N}\t\n\r]/)), 'sym'),
     // for checkboxes: ugly, but makes them work..
    // alias(tfunc(prec(pr, 'x')), 'str'),
    // alias(tfunc(prec(pr, 'X')), 'str'),
  )
}

function sp_imm(item) { return token(prec.left('special', item)); }
function sp_nim(item) { return token.immediate(prec('special', item)); }
function nim(item) { return token(prec('non-immediate', item)); }
function imm(item) { return token.immediate(prec('immediate', item)); }
function prim_1(item) {
  return token.immediate(prec(1, item));
}
