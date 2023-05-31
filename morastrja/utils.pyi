from __future__ import annotations

from typing import overload
from . import MoraStr


@overload
def vowel_to_choon(__kana_string: str, maxrep: int = 1,
                   *, clean: bool = False,
                   ou: bool = False, ei: bool = False,
                   nn: bool = True) -> str: ...
@overload
def vowel_to_choon(__kana_string: MoraStr, maxrep: int = 1,
                   *, clean: bool = False,
                   ou: bool = False, ei: bool = False,
                   nn: bool = True) -> MoraStr:
    "convert consecutive vowels in kana_string to choonpu"


@overload
def choon_to_vowel(__kana_string: str,
                   *, strict: bool = True,
                   clean: bool = False) -> str: ...
@overload
def choon_to_vowel(__kana_string: MoraStr,
                   *, strict: bool = True,
                   clean: bool = False) -> MoraStr:
    "convert choonpu in kana_string to vowel characters"
