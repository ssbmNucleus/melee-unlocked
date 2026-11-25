"""Helpers for source-level integration checks."""
import re


def body(text, name):
    # Balanced braces, excluding comments and strings from the brace scan.
    mask = re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
                  lambda m: ' ' * len(m[0]), text, flags=re.S)
    # A definition starts its line; a call such as "switch (g_warmup.tick(o)) {" does not. Fall back to
    # the loose form for definitions written indented (inside a namespace or class).
    match = (re.search(r'(?m)^[A-Za-z_][^\n;=]*?\b' + name + r'\([^;]*?\)\s*(?:const\s*)?\{', mask) or
             re.search(r'\b' + name + r'\([^;]*?\)\s*\{', mask))
    if not match:
        raise AssertionError('Missing function: ' + name)
    depth, at = 1, match.end()
    while depth:
        depth += (mask[at] == '{') - (mask[at] == '}')
        at += 1
    return text[match.end():at - 1]
