import io.github.treesitter.jtreesitter.Language;
import io.github.treesitter.jtreesitter.fey.TreeSitterFey;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;

public class TreeSitterFeyTest {
    @Test
    public void testCanLoadLanguage() {
        assertDoesNotThrow(() -> new Language(TreeSitterFey.language()));
    }
}
