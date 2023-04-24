from setuptools import setup, Extension


ext = Extension('morastrja._morastr',
                sources = ['ext/cmorastr.c'],
                depends = ['*.h', 'cmorastr_twoway.c'],
                extra_compile_args=['-O2'])

setup (name = 'morastrja',
       version = '0.8.8',
       description = 'Mora String for the Japanese Language',
       author = 'Hizuru',
       url = 'https://github.com/Hizuru3/morastrja',
       license = 'MIT',
       long_description = '''
This module provides a class that counts morae, based on Japanese syllabaries.
''',
       packages = ['morastrja', 'morastrja.data'],
       package_data = {'morastrja': ['py.typed', '__init__.pyi', 'utils.pyi'],
                       'morastrja.data': ['table.bak']},
       ext_modules = [ext],
       classifiers = ['Intended Audience :: Science/Research',
                      'Intended Audience :: Developers',
                      'License :: OSI Approved :: MIT License',
                      'Natural Language :: Japanese',
                      'Programming Language :: Python :: 3.7',
                      'Programming Language :: Python :: 3.8',
                      'Programming Language :: Python :: 3.9',
                      'Programming Language :: Python :: 3.10',
                      'Programming Language :: Python :: 3.11',
                      'Programming Language :: Python :: Implementation :: CPython',
                      'Topic :: Text Processing :: Linguistic'],
       python_requires = '>=3.7')