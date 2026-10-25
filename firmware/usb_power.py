"""Build-local descriptor patch: bus powered, request 500 mA after enumeration.
The SDK is never modified. Fail closed if the expected descriptor layout changes.
Current consumption and pre-enumeration current still require hardware tests.
"""
from pathlib import Path
import re

Import('env')


def remix_usb_descriptor(env, node):
    if node.name != 'usb_desc.c':
        return node
    source = Path(node.srcnode().get_abspath())
    text = source.read_text(encoding='utf-8')
    text, attributes = re.subn(r'0xC0,(\s*// bmAttributes)', r'0x80,\1', text)
    text, powers = re.subn(r'50,(\s*// bMaxPower)', r'250,\1', text)
    if attributes != 2 or powers != 2:
        raise RuntimeError('USB descriptor structure changed; inspect both speed configurations')
    target = Path(env.subst('$BUILD_DIR')) / 'remix_usb_desc.c'
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() or target.read_text(encoding='utf-8') != text:
        target.write_text(text, encoding='utf-8')
    print('ReMix USB: bus powered, bMaxPower=250 (500 mA); both speed configurations')
    return env.File(str(target))


env.AddBuildMiddleware(remix_usb_descriptor, '*usb_desc.c')
