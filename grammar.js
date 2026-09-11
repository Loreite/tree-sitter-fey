/**
 * @file A parser for the configurable Fey markup language
 * @author qaptoR <git@arxdaingneachd.org>
 * @license MIT
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

// grammar.js
export default grammar({
  name: "fey",

  extras: _ => [/[ \f\t\v\u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000\ufeff]/],

  externals: $ => [
    $._line_content,
    $._section_start,
    $._section_end,
  ],

  rules: {
    document: $ => seq(
      field('preamble', seq(
        repeat($._nl),
        repeat($.content)
      )),
      repeat($.subsection),
    ),

    content: $ => seq(
      $.paragraph,
      repeat($._nl),
    ),

    subsection: $ => seq(
      $._section_start,
      $.heading,
      optional($.body),
      repeat($.subsection),
      $._section_end,
    ),

    heading: $ => seq(
      '  ',
      field('prefix', $.heading_prefix),
      field('title', $.title),
      repeat($._nl),
    ),

    body: $ => repeat1($.content),

    heading_prefix: $ => repeat1(
      seq(
        /[a-zA-Z0-9_<>{}()\[\]]*/,
        /[.,:;/\\!?'"\-+*=@&#$%]/,
      )
    ),

    title: $ => seq(/[ \t]+/, /[^\r\n]*/),

    paragraph: $ => prec.right(
      repeat1(seq(
        $._line_content,
        $._nl,
      )),
    ),

    _nl: $ => /\r?\n/,
  }
});
