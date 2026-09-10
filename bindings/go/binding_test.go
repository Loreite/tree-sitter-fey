package tree_sitter_fey_test

import (
	"testing"

	tree_sitter "github.com/tree-sitter/go-tree-sitter"
	tree_sitter_fey "github.com/Loreite/tree-sitter-fey/bindings/go"
)

func TestCanLoadGrammar(t *testing.T) {
	language := tree_sitter.NewLanguage(tree_sitter_fey.Language())
	if language == nil {
		t.Errorf("Error loading Fey grammar")
	}
}
