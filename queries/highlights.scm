;; Level-based headline highlighting using a custom predicate matcher
(heading (heading_prefix) @prefix (#fey-is-heading-level? @prefix "1")) @org.headline.level1
(heading (heading_prefix) @prefix (#fey-is-heading-level? @prefix "2")) @org.headline.level2
(heading (heading_prefix) @prefix (#fey-is-heading-level? @prefix "3")) @org.headline.level3
(heading (heading_prefix) @prefix (#fey-is-heading-level? @prefix "4")) @org.headline.level4
(heading (heading_prefix) @prefix (#fey-is-heading-level? @prefix "5")) @org.headline.level5
(heading (heading_prefix) @prefix (#fey-is-heading-level? @prefix "6")) @org.headline.level6
(heading (heading_prefix) @prefix (#fey-is-heading-level? @prefix "7")) @org.headline.level7
(heading (heading_prefix) @prefix (#fey-is-heading-level? @prefix "8")) @org.headline.level8
(body (content (paragraph) @spell))

