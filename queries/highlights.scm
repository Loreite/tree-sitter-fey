;; The highlights of this grammar for tools that only have the grammar (the editor plugin has its own, richer set in
;; `queries/fey/highlights.scm` of fey.nvim, which colours by tag form and by the names of tags).
(heading signature: (signature) @markup.heading.marker)
(heading title: (title) @markup.heading)
(tag_name) @tag
(tag_start) @punctuation.bracket
(tag_end) @punctuation.bracket
(key) @property
(paragraph) @spell
