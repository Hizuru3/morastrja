import doctest
import sys

import morastrja


globs = dict(vars(morastrja))
globs.update(vars(morastrja.utils))

flags = (doctest.IGNORE_EXCEPTION_DETAIL
       | doctest.REPORT_NDIFF
       | doctest.FAIL_FAST)

targets = [r"README.md", r"docs/_sources/morastr_module.rst.txt"]

for target in targets:
    failures, _ = doctest.testfile(
        target, globs=globs, optionflags=flags)

    if failures: sys.exit(1)
