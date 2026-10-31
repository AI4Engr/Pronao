#!/usr/bin/env python3
"""The table of pointer kinds in the C++ the compiler wrote: for every class member (the header) and for every function (the .cpp) the lines that spell
std::unique_ptr / std::shared_ptr / std::weak_ptr, with line numbers and closure numbers taken out.
    ptr_table.py table WORKDIR out.txt [module ...]          write the table of those modules (default: every module in WORKDIR that is not cjson or a test main)
    ptr_table.py diff BASE.txt OTHER.txt                     what is in one and not in the other; exit 1 if they differ
    ptr_table.py bodies WORKDIR out.txt [module ...]         one line per function: its name and a hash of its whole generated text (line numbers and closure numbers
                                                             taken out); diff BASE.txt OTHER.txt then names the functions whose generated code changed. Stricter than the
                                                             table: moves, copies, const& parameters and every pointer kind are in the text of a function.

Used to check that moving code between files (src/app.clg into several modules) changes no pointer kind: pointer kinds are inferred per file, so a class that
moves to a file of its own can be inferred differently, and this is how that is noticed. A line is "class member" or "in function F".
"""
import collections
import os
import re
import sys

PTR = re.compile(r'std::(unique|shared|weak)_ptr<')


def normalize(line):
    line = line.strip()
    line = re.sub(r'"line \d+"', '"line N"', line)
    line = re.sub(r'(cling::at\([^;]*?), \d+\)', r'\1, N)', line)
    line = re.sub(r'__closure_[A-Za-z0-9]+_\d+', '__closure', line)
    line = re.sub(r'\s+', ' ', line)
    return line


def header_rows(path):
    rows = []
    cls = None
    depth = 0
    for raw in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'\s*(?:class|struct)\s+([A-Za-z0-9_]+)\b[^;]*\{\s*$', raw)
        if cls is None and m:
            cls = re.sub(r'__closure_[A-Za-z0-9]+_\d+', '__closure', m.group(1))          # (closure classes are numbered per file: they are one kind here)
            depth = 1
            continue
        if cls is not None:
            depth += raw.count('{') - raw.count('}')
            if depth <= 0:
                cls = None
                continue
            if PTR.search(raw):
                rows.append(('member of ' + cls, normalize(raw)))
    return rows


FUNC = re.compile(r'^(?:[A-Za-z_][A-Za-z0-9_:<>\*&, ]*?\s+)?((?:[A-Za-z_][A-Za-z0-9_]*::)*[A-Za-z_~][A-Za-z0-9_]*)\s*\(')


def cpp_rows(path):
    """The pointer-kind lines of each function. The closure classes the compiler writes into the .cpp are skipped (their members are in the table of the file that
    has the closure, and their numbering changes whenever code moves); so are declarations that are not function definitions."""
    rows = []
    fn = None
    in_closure_class = False
    for raw in open(path, encoding='utf-8', errors='replace'):
        if re.match(r'(?:class|struct)\s+__closure', raw):
            in_closure_class = True
        if in_closure_class:
            if raw.startswith('};') or raw.startswith('#endif'):
                in_closure_class = False
            continue
        if raw and not raw[0].isspace() and not raw.startswith(('#', '}', '//', 'static', 'template', 'using', 'namespace', 'extern', 'class', 'struct')):
            stripped = raw.rstrip()
            if stripped.endswith('{') or (stripped.endswith(':') and '(' in raw) or ') :' in raw:
                m = FUNC.match(raw)
                if m:
                    fn = re.sub(r'__closure_[A-Za-z0-9]+_\d+', '__closure', m.group(1))
        if PTR.search(raw) and fn is not None:
            rows.append(('in ' + fn, normalize(raw)))
    return rows


def function_bodies(path):
    """name -> the normalized lines of every function defined in a .cpp (closure classes included, under the name __closure::method)."""
    out = collections.defaultdict(list)             # name -> a list of definitions, each a list of lines (closures, overloads: several under one name)
    fn = None
    current = None
    for raw in open(path, encoding='utf-8', errors='replace'):
        if raw and not raw[0].isspace() and not raw.startswith(('#', '}', '//', 'static', 'template', 'using', 'namespace', 'extern', 'class', 'struct')):
            stripped = raw.rstrip()
            if stripped.endswith('{') or ') :' in raw or (stripped.endswith(':') and '(' in raw):
                m = FUNC.match(raw)
                if m:
                    fn = re.sub(r'__closure_[A-Za-z0-9]+_\d+', '__closure', m.group(1))
                    current = []
                    out[fn].append(current)
        if fn is not None and raw.strip():
            current.append(normalize(raw))
            if raw.startswith('}'):
                fn = None                                   # the function ends at its closing brace in column 0: what follows is not part of it
    return out


def bodies(work, modules, out):
    import hashlib
    if not modules:
        modules = sorted({os.path.splitext(f)[0] for f in os.listdir(work) if f.endswith('.cpp')} - {'embedded'})
        modules = [m for m in modules if not m.startswith('cjson_') and not m.endswith('_main')]
    merged = collections.defaultdict(list)
    for m in modules:
        c = os.path.join(work, m + '.cpp')
        if os.path.exists(c):
            for name, definitions in function_bodies(c).items():
                for body in definitions:
                    merged[name].append('\n'.join(body))
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        for name in sorted(merged):
            distinct = sorted(set(merged[name]))              # (a helper every .cpp carries a copy of, or the closures of one name: the DISTINCT texts are what matters)
            digest = hashlib.sha1('\n--\n'.join(distinct).encode('utf-8')).hexdigest()[:12]
            f.write('%s | %s | %d\n' % (name, digest, len(distinct)))
    print('%d functions from %d modules' % (len(merged), len(modules)))


def table(work, modules, out):
    if not modules:
        modules = sorted({os.path.splitext(f)[0] for f in os.listdir(work) if f.endswith('.cpp')} - {'embedded'})
        modules = [m for m in modules if not m.startswith('cjson_') and not m.endswith('_main')]
    rows = []
    for m in modules:
        h, c = os.path.join(work, m + '.h'), os.path.join(work, m + '.cpp')
        if os.path.exists(h):
            rows += header_rows(h)
        if os.path.exists(c):
            rows += cpp_rows(c)
        if m.endswith('_main'):
            continue
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        for where, line in sorted(rows):
            f.write('%s | %s\n' % (where, line))
    print('%d rows from %d modules: %s' % (len(rows), len(modules), ' '.join(modules)))


def diff(a, b):
    def load(p):
        c = collections.Counter()
        for line in open(p, encoding='utf-8'):
            c[line.rstrip('\n')] += 1
        return c
    ca, cb = load(a), load(b)
    only_a = ca - cb
    only_b = cb - ca
    for line in sorted(only_a):
        print('- %s%s' % (line, '' if only_a[line] == 1 else '   (x%d)' % only_a[line]))
    for line in sorted(only_b):
        print('+ %s%s' % (line, '' if only_b[line] == 1 else '   (x%d)' % only_b[line]))
    print('%d only in %s, %d only in %s' % (sum(only_a.values()), os.path.basename(a), sum(only_b.values()), os.path.basename(b)))
    return 0 if not only_a and not only_b else 1


if __name__ == '__main__':
    if len(sys.argv) >= 4 and sys.argv[1] == 'table':
        table(sys.argv[2], sys.argv[4:], sys.argv[3])
    elif len(sys.argv) >= 4 and sys.argv[1] == 'bodies':
        bodies(sys.argv[2], sys.argv[4:], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == 'diff':
        sys.exit(diff(sys.argv[2], sys.argv[3]))
    else:
        print(__doc__)
        sys.exit(2)
