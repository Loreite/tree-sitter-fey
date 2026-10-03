#ifndef HTML_NAMES_H_
#define HTML_NAMES_H_

// Sorted (strcmp order) so the scanner can binary-search it.
static const char *const HTML_NAMES[] = {
    "a",          "abbr",      "acronym",  "address",  "applet",   "area",
    "article",    "aside",     "audio",    "b",        "base",     "basefont",
    "bdi",        "bdo",       "bgsound",  "big",      "blink",    "blockquote",
    "body",       "br",        "button",   "canvas",   "caption",  "center",
    "cite",       "code",      "col",      "colgroup", "data",     "datalist",
    "dd",         "del",       "details",  "dfn",      "dialog",   "dir",
    "div",        "dl",        "dt",       "em",       "embed",    "fieldset",
    "figcaption", "figure",    "font",     "footer",   "form",     "frame",
    "frameset",   "h1",        "h2",       "h3",       "h4",       "h5",
    "h6",         "head",      "header",   "hgroup",   "hr",       "html",
    "i",          "iframe",    "img",      "input",    "ins",      "isindex",
    "kbd",        "keygen",    "label",    "legend",   "li",       "link",
    "listing",    "main",      "map",      "mark",     "marquee",  "math",
    "menu",       "menuitem",  "meta",     "meter",    "multicol", "nav",
    "nextid",     "nobr",      "noembed",  "noframes", "noscript", "object",
    "ol",         "optgroup",  "option",   "output",   "p",        "param",
    "picture",    "plaintext", "pre",      "progress", "q",        "rb",
    "rp",         "rt",        "rtc",      "ruby",     "s",        "samp",
    "script",     "search",    "section",  "select",   "slot",     "small",
    "source",     "spacer",    "span",     "strike",   "strong",   "style",
    "sub",        "summary",   "sup",      "svg",      "table",    "tbody",
    "td",         "template",  "textarea", "tfoot",    "th",       "thead",
    "time",       "title",     "tr",       "track",    "tt",       "u",
    "ul",         "var",       "video",    "wbr",      "xmp",
};

#define HTML_NAMES_COUNT 143
_Static_assert(HTML_NAMES_COUNT == sizeof(HTML_NAMES) / sizeof(HTML_NAMES[0]),
               "HTML_NAMES_COUNT out of sync with HTML_NAMES");
_Static_assert(HTML_NAMES_COUNT < 255, "Too many HTML names");

#endif // HTML_NAMES_H_
