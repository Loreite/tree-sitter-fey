#!/usr/bin/env bash

tree-sitter generate && \
gcc -o fey.so -shared src/parser.c src/scanner.c -Os -I./src -fPIC
