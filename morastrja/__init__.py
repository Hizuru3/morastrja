from ._morastr import MoraStr, count_all


__all__ = ['MoraStr', 'count_all', 'CONVERSION_TABLE', 'utils',]


def _init():
    from .data import table
    from . import _morastr

    result = mapping = {}
    for name, var in vars(table).items():
        if isinstance(var, dict) and not name.startswith('__'):
            mapping.update(var)

    mapping = _morastr._register(mapping)
    if mapping:
        import re
        from functools import partial

        pattern = re.compile('({})'.format('|'.join(
            map(re.escape, sorted(mapping, key=len, reverse=True)) )))
        converter = partial(pattern.sub, lambda m: mapping[m[0]])
        _morastr._set_converter(converter)

    return result


try:
    CONVERSION_TABLE = type(MoraStr.__dict__)(_init())
finally:
    del _init

from . import utils
