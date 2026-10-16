#!/usr/bin/env python3
"""Extract a local generated reference for tests. Never distribute the output."""
import pathlib,re,sys
source=pathlib.Path(sys.argv[1]); target=pathlib.Path(sys.argv[2])
for path in sorted(source.glob('rec_*.c')):
    text=path.read_text()
    match=re.search(r'void f_0039AFB0(?:_orig)?\(void\) \{',text)
    if not match: continue
    start=match.start(); pos=match.end(); depth=1
    while depth:
        if text[pos]=='{': depth+=1
        elif text[pos]=='}': depth-=1
        pos+=1
    function=text[start:pos]
    function=re.sub(r'f_0039AFB0(?:_orig)?(?=\()', 'reference_movie_bits_advance',function,count=1)
    target.write_text('/* Local disc-derived test reference; never commit. */\n#include "ee.h"\n'
        'void movie_bits_test_safe_point(void);\n#undef IRQ_CHECK\n'
        '#define IRQ_CHECK() do { movie_bits_test_safe_point(); if (g_ee_irq_pending) ee_service_interrupts(); } while (0)\n'
        + function + '\n')
    break
else: raise SystemExit('Generate rec_*.c before building movie bit-buffer tests')
