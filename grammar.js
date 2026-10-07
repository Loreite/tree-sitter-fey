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
const reTagName = /[a-zA-Z_][a-zA-Z0-9_]*/;
const reTagKey = reTagName;
const reTagBracket = /[\[\]{}()<>]/;
const reTagOpenBracket = /[\[{(<]/;
const reTagCloseBracket = /[\]})>]/;
const reTagToken =/[.,:;!?\\/\-+*=~^%@&#$]/
const reTagOpen = /[\[{(<][.,:;!?\\/\-_+*=~^%@&#$]/;
const reTagClose = /[.,:;!?\\/\-_+*=~^%@&#$][\]})>]/;

// Tags require a space/tab between their open/close markers and their content:
//   [# name #]    {$ name, value $}    ( name )#    *[ name ]  body *
const reWs = /[ \t]/;
const reWsPlus = /[ \t]+/;

const reCellEscape = /\\[|.,:;!?\\/\-+*=~^%@&#$]/;
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
    // Pair tags. All four are decided by scanner lookahead; the first three
    // are zero-width (the grammar lexes the brackets), the stray closer
    // covers the whole mismatched `[# name ]`.
    $._pair_open_start,   // inline opener `[ name #]` (pushes the name)
    $._pair_block_start,  // block opener: `[ name #]` then end of line
    $._pair_close_start,  // closer `[# name ]` matching the innermost opener (pops)
    $._pair_stray_close,  // closer whose name/bracket/token does not match
    // Line break between two words of a multi-line tag value. The scanner
    // only emits it when the next line does NOT start with a tag closer
    // (`]`, `#]`, `]#`, ...); otherwise it emits _tag_nl so tag_close wins.
    $._tag_value_nl,
    // Zero-width close of the innermost pair when its closer is missing:
    // emitted before a closer that matches an OUTER opener (HTML-style
    // implicit end), before a heading line, and at end of input.
    $._pair_implicit_close,
    // Zero-width: "the line ends here" (next char is \r, \n or EOF).
    // Ends a line_tag without stealing the newline from its paragraph.
    $._line_tag_eol,
    // Zero-width gates. The tag rules below are bracket-agnostic (and the
    // scope tags sigil-agnostic too) to keep the parse table small; these
    // gates are only emitted after the scanner has checked that the closer
    // matches the opener, so a mismatched tag never starts.
    $._line_tag_start,        // `#[ name ... ]`   closer bracket matches
    $._scope_tag_start,       // `[# name ... #]`  closer sigil + bracket match
    $._scope_line_tag_start,  // same, closer on the same line (table cells)
    // The end of the head of a scope tag or a pair opener: the sigil and the
    // bracket of its opening, after a blank (`_head_end`) or at the start of a
    // line after a line break (`_head_end_bare`). The scanner remembers the
    // opening, so any other sign and bracket (`->`, `:)`) is a word.
    $._head_end,
    $._head_end_bare,
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
      alias($._block_pair_tag, $.pair_tag),
      $._standalone_scope_tag,
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
      // Two blanks separate the bullet from its contents. An empty listitem
      // (`key_:` or `-` then end of line) has no separator: its contents, if
      // any, start with the newline (e.g. a sublist on the following lines).
      // The scanner only emits _bullet for these two shapes.
      optional(/[ \t]{2,}/),
      optional(field('checkbox', $.checkbox)),
      choice(
        $._eof,
        field('contents', $._body_contents),
      ),
    ),

    // `[ ]` open, `[x]` done, and any other single mark that is not a digit or a bracket, `[/]` `[!]` `[n]` ...:
    // the first thing of an item's contents. What a mark means is decided by the plugin, the grammar only
    // knows the shape. A pair tag opener is `[ name #]`, which the scanner only emits when the name and the
    // closing token are there, and a cookie or a reference has digits, so they never compete.
    checkbox: $ => token(prec(2, /\[([ ]|[^\[\]\s0-9])\]/)),

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

    // One alternative per SIGIL (19): the body and terminator depend on it.
    // The bracket is agnostic: one shared head whose closer is any close
    // bracket. _line_tag_start is only emitted when the closer matches the
    // opening bracket (scanner.c, scan_line_tag_head).
    line_tag: $ => choice(
      ...tagTokens.map((tok, ti) => seq(
        $._line_tag_start,
        field('tag_closure', alias(
          token(seq(sp_nim(tok), imm(reTagOpenBracket), imm(reWs))), $.tag_start)
        ),
        $._line_tag_head,
        $[`_line_tag_tail_${ti}`],
      )),
    ),

    _line_tag_head: $ => seq(
      $._tag_head,
      head_close($),
    ),

    // The head of every tag form: a name, values after commas, keys after
    // semicolons. What ends it is not the grammar's business: the scanner
    // recognises the tag first and remembers how it was opened, and only the end
    // that belongs to the opening (`_head_end`) closes the head. Every other
    // sign and bracket is a word, so `{@ link, a; desc: x > y -> z @}` is a link
    // whose description is `x > y -> z`.
    _tag_head: $ => seq(
      repeat($._tag_nl),
      field('name', alias(nim(reTagName), $.tag_name)),
      repeat($._tag_multi_value_choice),
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

    _tag_key_value: $ => seq(
      field('key', alias(nim(reTagKey), $.key)),
      alias(nim(/[.:=]/), 'tag_delimiter'),
      field('value', alias($._tag_value, $.value)),
    ),

    _tag_value: $ => seq(
      tag_word(',;'),
      repeat(seq(optional($._tag_value_nl), tag_word(',;'))),
    ),

    // one per sigil: body and terminator depend only on the sigil
    ...Object.fromEntries(tagTokens.flatMap((tok, ti) => [
      [`_line_tag_body_${ti}`, $ => line_tag_body(tok, $)],
      [`_line_tag_tail_${ti}`, $ => seq(
        // One blank separates head from body. The separator may also be
        // followed directly by the terminator (`#[ b ] #`): the lexer commits
        // to the separator before it can see that no body follows.
        optional(seq(
          /[ \t]+/,
          optional(alias($[`_line_tag_body_${ti}`], $.body)),
        )),
        choice(
          $._line_tag_eol,
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
        seq(/[ \t]+/, field('body', $.body)),
      ),
      $._block_tag_end,
    ),

    // Bracket-agnostic: _block_tag_start is only emitted after the scanner
    // has checked that the closer matches the opener (scan_open_shape).
    _block_tag_open: $ => seq(
      field('tag_closure', alias(
        token.immediate(prec('special', seq(reTagOpenBracket, reWs))), $.tag_start)),
      repeat($._tag_nl),
      field('name', alias(nim(reTagName), $.tag_name)),
      repeat($._tag_multi_value_choice),
      optional(alias(choice(nim(','), nim(';')), 'tag_delimiter')),
      head_close($),
    ),

    // ---- Pair tags --------------------------------------------------------
    //
    //   text [ name, v; k=v #] inline body [# name ] more text
    //
    //   [ name #]          opener ends its line: same node, entered as an
    //   body...            element so it can sit between other elements
    //   [# name ]
    //
    // The body is INDENTATION-AGNOSTIC: the scanner gives every open pair a
    // fresh indentation root (see PairEntry floors in scanner.c), so a list,
    // table, block tag or fenced block inside the body is laid out as if at
    // column 0, whatever container the opener sits in. Only the opener and
    // closer lines take part in the surrounding indentation.
    //
    // The scanner keeps a stack of open pair names and only emits
    // _pair_close_start when the closer matches the innermost opener (same
    // name, bracket and tag token). A closer that matches an outer opener
    // first implicitly closes the inner ones (_pair_implicit_close), as do a
    // heading line and the end of input.

    pair_tag: $ => seq(
      field('open', $.pair_open),
      optional(field('body', alias($._pair_body, $.body))),
      field('close', $._pair_close_any),
    ),

    _block_pair_tag: $ => seq(
      field('open', alias($._pair_block_open, $.pair_open)),
      optional(field('body', alias($._pair_body, $.body))),
      field('close', $._pair_close_any),
    ),

    _pair_close_any: $ => choice(
      $.pair_close,
      alias($._pair_implicit_close, $.implicit_close),
    ),

    // Starts mid-line (after the opener) and may end mid-line (before the
    // closer), with any block content in between. As in the body of a section,
    // an element (a table, a list, a fenced block, a block tag, ...) can only
    // follow a line break: a paragraph that does not end in one is the last
    // thing before the closer, so `a | b` is a paragraph and not a paragraph
    // followed by the table row `| b`.
    _pair_body: $ => choice(
      seq(
        repeat1(choice(
          alias($._pair_paragraph, $.paragraph),
          $._nl,
          $._element,
        )),
        optional(alias($._pair_paragraph_tail, $.paragraph)),
      ),
      alias($._pair_paragraph_tail, $.paragraph),
    ),

    // A paragraph whose last line ends in a line break. prec.right: a newline
    // after a line always belongs to the paragraph, which can still end there.
    _pair_paragraph: $ => prec.right(seq(
      $._inline_item,
      repeat(choice($._inline_item, seq($._nl, $._inline_item))),
      $._nl,
    )),

    // The last paragraph of a body: its last line is followed by the closer on
    // the same line (or by the closer's own line, the break being left to the
    // body).
    _pair_paragraph_tail: $ => seq(
      $._inline_item,
      repeat(choice($._inline_item, seq($._nl, $._inline_item))),
    ),

    pair_open: $ => seq($._pair_open_start, $._pair_open_head),
    _pair_block_open: $ => seq($._pair_block_start, $._pair_open_head),

    // Bracket-agnostic on purpose: one regex token for the open bracket and
    // one for `<ws><token><close>`. Spelling out the 76 bracket x token
    // combinations (as scope_multi_tag does) overflows tree-sitter's 65535
    // parse-action limit. The scanner already verified that the closer
    // matches the opening bracket before emitting _pair_open_start /
    // _pair_block_start, so the grammar does not need to repeat it.
    _pair_open_head: $ => seq(
      field('tag_closure', alias(
        token(prec('special', seq(reTagOpenBracket, reWs))), $.tag_start)),
      $._tag_head,
      head_close($),
    ),

    pair_close: $ => seq(
      $._pair_close_start,
      field('tag_closure', alias(scope_tag_start(), $.tag_start)),
      field('name', alias(nim(reTagName), $.tag_name)),
      field('tag_closure', alias(ws_end(reTagCloseBracket), $.tag_end)),
    ),

    stray_close: $ => $._pair_stray_close,

    _standalone_scope_tag: $ => seq(
      $._standalone_tag_start,
      alias($._scope_tag_inner, $.scope_tag),
      $._eol,
    ),

    // Sigil- and bracket-agnostic, like _pair_open_head: spelling out the 76
    // combinations cost ~20k parse actions. The scanner checks that the
    // closer's sigil and bracket match the opener before it emits
    // _scope_tag_start / _standalone_tag_start / _scope_line_tag_start.
    scope_multi_tag: $ => seq($._scope_tag_start, $._scope_tag_inner),

    _scope_tag_inner: $ => seq(
      field('tag_closure', alias(scope_tag_start(), $.tag_start)),
      $._tag_head,
      head_close($),
    ),

    scope_line_tag: $ => seq(
      $._scope_line_tag_start,
      field('tag_closure', alias(scope_tag_start(), $.tag_start)),
      $._tag_line_head,
      field('tag_closure', alias($._head_end, $.tag_end)),
    ),

    // Single-line counterpart of _tag_head, shared by every scope_line_tag.
    _tag_line_head: $ => seq(
      field('name', alias(nim(reTagName), $.tag_name)),
      repeat($._tag_line_value_choice),
      optional(alias(choice(nim(','), nim(';')), 'tag_delimiter')),
    ),

    _tag_line_value_choice: $ => choice(
      seq(
        alias(nim(','), 'tag_delimiter'),
        field('value', alias($._tag_line_value, $.value))
      ),
      seq(
        alias(nim(';'), 'tag_delimiter'),
        field('key_value', alias($._tag_line_key_value, $.value))
      ),
    ),

    _tag_line_key_value: $ => seq(
      field('key', alias(nim(reTagKey), $.key)),
      alias(nim(/[.:=]/), 'tag_delimiter'),
      field('value', alias($._tag_line_value, $.value)),
    ),

    // Single-line value, for scope_line_tag (table cells).
    _tag_line_value: $ => repeat1(tag_word(',;')),

    _expr_line: $ => repeat1($.expr),

    _tagged_cell_line: $ => repeat1(choice(
      alias($.cell_expr, $.expr), alias($.scope_line_tag, $.scope_tag)
    )),

    // _tagged_expr_line: $ => repeat1(choice(
    //   $.expr,
    //   alias($.scope_line_tag, $.scope_tag),
    // )),

    _tagged_expr_multi_line: $ => repeat1($._inline_item),

    // Anything that can appear in running (paragraph/title) text.
    _inline_item: $ => choice(
      $.expr,
      $.line_tag,
      alias($.scope_multi_tag, $.scope_tag),
      $.pair_tag,
      $.stray_close,
    ),

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

// End of the head of a tag, scanned against its opening (see `_head_end`):
// after a blank, or, for a multi-line head, on a line of its own after the
// line breaks (the external _tag_nl already consumes the indentation).
function head_close($) {
  return choice(
    field('tag_closure', alias($._head_end, $.tag_end)),
    seq(
      repeat1($._tag_nl),
      field('tag_closure', alias($._head_end_bare, $.tag_end)),
    ),
  );
}

// Closer token that must be preceded by at least one space/tab.
function ws_end(...parts) {
  return token.immediate(prec('special', seq(reWsPlus, ...parts)));
}

// `[# ` opener shared by scope tags and pair closers (one lexer token).
function scope_tag_start() {
  return token(prec('special', seq(reTagOpenBracket, reTagToken, reWs)));
}

function line_tag_body(skip, $) {
  return repeat1(choice(
    tag_word(skip), $.line_tag,
    alias($.scope_multi_tag, $.scope_tag),
    $.pair_tag, $.stray_close,
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
