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

export default grammar({
  name: "fey",

  extras: _ => [/[ \f\t\v\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000\ufeff]/],

  externals: $ => [
    $._list_start,
    $._list_end,
    $._listitem_end,
    $._bullet,
    // $._signature_segment,
    // $._liststart_segment,
    // $._bullet_segment,
    $._two_spaces,
    $._signature,
    $._section_end,
    $._eof,  // Basically just '\0', but allows multiple to be matched
  ],


  inline: $ => [
    // $._nl,
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
      // $.table,
      // $.block
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
      /[ \t]+/,
      optional(field('title', $.title)),
      $._eol,
      // repeat($._nl),
    ),

    signature: $ => seq(
      // $._signature_segment,
      $._signature,
      '  ',
      repeat1($.segment),
    ),

    segment: $ => seq(
      alias(/[a-zA-Z0-9_]*/, 'index'),
      alias(token.immediate(/[.,:;!?/\\'"`\-+*=~^%@&#$\[\](){}<>]/), 'delim'),
    ),

    // title: $ => seq(/[ \t]+/, /[^\r\n]*/),
    title: $ => repeat1($.expr),

    paragraph: $ => seq(
      // optional($._directive_list),
      $._multiline_text
    ),

    list: $ => seq(
      // optional($._directive_list),
      // $._liststart_segment,
      $._list_start,  // captures indent length and bullet type
      repeat(seq($.listitem, $._listitem_end, repeat($._nl))),
      seq($.listitem, $._list_end)
    ),

    listitem: $ => seq(
      field('bullet', $.bullet),
      $._two_spaces, /[ \t]{2,}/,
      // optional(field('checkbox', $.checkbox)),
      choice(
        $._eof,
        field('contents', $._body_contents),
      ),
    ),

    bullet: $ => seq(
      // $._bullet_segment,
      $._bullet,
      $.segment,
    ),

    _multiline_text: $ => repeat1(
      seq(repeat1($.expr), $._eol)
    ),

    expr: $ => seq(
      expr('non-immediate', token),
      repeat(expr('immediate', token.immediate))
    ),

    _nl: _ => /\r\n|\r|\n/,
    _eol: $ => choice($._nl, $._eof),
  }
});

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
