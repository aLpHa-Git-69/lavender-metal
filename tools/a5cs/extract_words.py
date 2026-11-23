"""Pull the machine-code words out of the fixture ir3_compiler -o wrote (fill_id_a5.hpp) into fill_id_a5_words.inc."""
import re, sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
m = re.search(r'fill_id_words\[\]\s*=\s*\{(.*?)\};', s, re.S)
ws = re.findall(r'0x[0-9a-f]+', m.group(1))[:64]
assert len(ws) == 64
open(dst, 'w').write(',\n'.join(ws) + ',\n')
print(len(ws), 'words')
