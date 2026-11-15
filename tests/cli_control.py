"""Makes a copy of the `pronao` command with one rule removed, for the controls of tests/run_cli.sh.
    python3 cli_control.py <pronao> <out> <pronao folder> <old text> <new text>
The copy finds its scripts in the real folder (ROOT is fixed to it), and the text to replace must be there exactly once: a control that changes nothing would pass for the wrong reason."""
import sys

src, dst, root, old, new = sys.argv[1:6]
s = open(src, encoding='utf-8', newline='').read()
s = s.replace('ROOT="$(cd "$(dirname "$SELF")" && pwd)"', 'ROOT="%s"' % root, 1)
if s.count(old) != 1:
    sys.exit('the text to replace is there %d times, not once: %s' % (s.count(old), old))
open(dst, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))
