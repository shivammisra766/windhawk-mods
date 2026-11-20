# Builds D:\lookup\wordnet.tsv for the Lookup popover from the WordNet 3.0 database
# (https://wordnetcode.princeton.edu/3.0/WNdb-3.0.tar.gz, extracted to D:\lookup\src-dl\dict).
# Run: python -I build-wordnet.py <dict dir> <out file>
# Lines, sorted by the UTF-8 bytes of the key:
#   word \t pos(n v a r) \t definition \t example \t synonyms (", ")   up to 4 senses per pos, commonest first
#   form \t = \t base                                                  irregular forms (from *.exc)
import os
import re
import sys

src, out = sys.argv[1], sys.argv[2]
POS = [('noun', 'n'), ('verb', 'v'), ('adj', 'a'), ('adv', 'r')]
MAX_SENSES = 4


def clean(w):
    return re.sub(r'\([a-z]+\)$', '', w).replace('_', ' ')


rows = []
for name, p in POS:
    synsets = {}
    with open(os.path.join(src, 'data.' + name), encoding='latin-1') as f:
        for line in f:
            if line.startswith('  '):
                continue
            head, _, gloss = line.partition(' | ')
            parts = head.split()
            off = parts[0]
            n = int(parts[3], 16)
            words = [clean(parts[4 + 2 * i]) for i in range(n)]
            gloss = gloss.strip()
            m = re.search(r';?\s*"', gloss)
            if m:
                definition = gloss[:m.start()].strip()
                ex = re.findall(r'"([^"]+)"', gloss[m.start():])
                example = ex[0].strip() if ex else ''
            else:
                definition, example = gloss, ''
            synsets[off] = (words, definition, example)
    with open(os.path.join(src, 'index.' + name), encoding='latin-1') as f:
        for line in f:
            if line.startswith('  '):
                continue
            parts = line.split()
            lemma = parts[0].replace('_', ' ')
            pcnt = int(parts[3])
            offs = parts[4 + pcnt + 2:]
            for off in offs[:MAX_SENSES]:
                words, d, ex = synsets[off]
                syns = [w for w in words if w.lower() != lemma.lower()][:5]
                rows.append((lemma, p, d, ex, ', '.join(syns)))
    with open(os.path.join(src, name + '.exc'), encoding='latin-1') as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2:
                rows.append((parts[0].replace('_', ' '), '=', parts[1].replace('_', ' '), '', ''))

order = {'n': 0, 'v': 1, 'a': 2, 'r': 3, '=': 4}
idx = {id(r): i for i, r in enumerate(rows)}  # keeps sense order within a key
rows.sort(key=lambda r: (r[0].encode('utf-8'), order[r[1]], idx[id(r)]))
seen = set()
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    for r in rows:
        if r[1] == '=':
            k = (r[0], r[2])
            if k in seen:
                continue
            seen.add(k)
            f.write(f'{r[0]}\t=\t{r[2]}\n')
        else:
            f.write('\t'.join(x.replace('\t', ' ') for x in r) + '\n')
print(len(rows), 'rows ->', out, os.path.getsize(out) // 1024, 'KB')
