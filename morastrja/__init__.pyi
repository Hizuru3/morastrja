from __future__ import annotations

import sys
from typing import TypeVar, overload, Any

if sys.version_info >= (3, 8):
    from typing import SupportsIndex
else:
    SupportsIndex = int

if sys.version_info >= (3, 9):
    from collections.abc import Sequence, Iterator, Iterable, Mapping
else:
    from typing import Sequence, Iterator, Iterable, Mapping

if sys.version_info >= (3, 11):
    from typing import Self
else:
    Self = TypeVar("Self", bound="MoraStr")


class MoraStr(Sequence[str]):
    @property
    def length(self) -> int:
        "Total count of morae. Same as len(morastr)."

    @property
    def string(self) -> str:
        "Underlying katakana representation as a plain str object."

    def __new__(cls: type[Self], __kana_string: str | MoraStr = '',
                *, ignore: bool = False) -> Self:
        """Create a sequence of morae from a Japanese kana string.
        
        The constructor takes at most 2 arguments, The first one 
        'kana_string' must be a string composed of Japanese kana syllabaries, 
        or an instance of the MoraStr class per se. If the input string 
        contains characters that are neither hiragana nor katakana, the 
        constructor raises a ValueError unless the keyword 'ignore' is set to 
        True, in which case, invalid characters are just skipped silently.
        
        Parameters
        ----------
        kana_string : str or MoraStr object
            Katakana or hiragana. (Full-width katakana are preferred)
        ignore : bool, optional
            Whether to skip invalid characters or not. Default is False.
        """

    def __add__(self: Self, __other: MoraStr | str) -> Self: ...

    def __contains__(self, __sub_morastr: str | MoraStr) -> bool: ...

    def __eq__(self, __other: object) -> bool: ...

    @overload
    def __getitem__(self: Self, __index: slice) -> Self: ...
    @overload
    def __getitem__(self, __index: int | SupportsIndex) -> str: ...

    def __getnewargs__(self) -> tuple[str]: ...

    def __hash__(self) -> int: ...

    def __iter__(self) -> Iterator[str]: ...

    def __len__(self) -> int: ...

    def __mul__(self: Self, __n: int | SupportsIndex) -> Self: ...

    def __rmul__(self: Self, __n: int | SupportsIndex) -> Self: ...

    def __ne__(self, __other: object) -> bool: ...

    def __repr__(self) -> str: ...

    def at(self, __i: int | SupportsIndex) -> str:
        "Return the ith mora (1-indexed), or an empty str if not found."

    def char_indices(self, *, zero: bool = False) -> list[int]:
        "Return a list of accumulative character counts for each mora."

    def count(self, __sub_morastr: str | MoraStr,
              __start: int | SupportsIndex | None = 0,
              __end: int | SupportsIndex | None = ...) -> int:
        "Count the occurences of sub_morastr within self[start:end]."

    def endswith(self, __suffix: str | MoraStr | tuple[str | MoraStr],
                 __start: int | SupportsIndex | None = 0,
                 __end: int | SupportsIndex | None = ...) -> bool:
        "Check if self[start:end] ends w/ suffix."

    @overload
    def find(self, __sub_morastr: str | MoraStr,
             __start: int | SupportsIndex | None = 0,
             __end: int | SupportsIndex | None = ...) -> int: ...
    @overload
    def find(self, __sub_morastr: str | MoraStr,
             *, charwise: bool = False) -> int: ...
    def find(self, *args, **kwargs):
        "Return the 1st index " \
        "where sub_morastr is found within self[start:end]."

    def finditer(self, __sub_morastr: str | MoraStr,
                 *, charwise: bool = False) -> Iterator[int]: ...
        "Return an iterator that yields indices of sub_morastr "
        "found in self."

    @overload
    def index(self, __sub_morastr: str | MoraStr,
              __start: int | SupportsIndex | None = 0,
              __end: int | SupportsIndex | None = ...) -> int: ...
    @overload
    def index(self, __sub_morastr: str | MoraStr,
              *, charwise: bool = False) -> int: ...
    def index(self, *args, **kwargs):
        "Like MoraStr.find(), but raises an error " \
        "when sub_morastr is not found."

    def removeprefix(self, __prefix: str | MoraStr) -> MoraStr:
        "Return self[len(prefix):] if self starts w/ prefix, " \
        "or a copy of self."

    def removesuffix(self, __suffix: str | MoraStr) -> MoraStr:
        "Return self[:len(self)-len(suffix)] if self ends w/ suffix, " \
        "or self[:]."

    def replace(self, __old: str | MoraStr, __new: str | MoraStr,
                __maxcount: int | SupportsIndex = -1) -> MoraStr:
        "Return a copy of self with the sub-morae 'old' " \
        "replaced by 'new'."

    @overload
    def rfind(self, __sub_morastr: str | MoraStr,
              __start: int | SupportsIndex | None = 0,
              __end: int | SupportsIndex | None = ...) -> int: ...
    @overload
    def rfind(self, __sub_morastr: str | MoraStr,
              *, charwise: bool = False) -> int: ...
    def rfind(self, *args, **kwargs):
        "Return the last index " \
        "where sub_morastr is found within self[start:end]."

    @overload
    def rindex(self, __sub_morastr: str | MoraStr,
               __start: int | SupportsIndex | None = 0,
               __end: int | SupportsIndex | None = ...) -> int: ...
    @overload
    def rindex(self, __sub_morastr: str | MoraStr,
               *, charwise: bool = False) -> int: ...
    def rindex(self, *args, **kwargs):
        "Like MoraStr.rfind(), but raise an error " \
        "when sub_morastr is not found."

    def startswith(self, __prefix: str | MoraStr | tuple[str | MoraStr],
                   __start: int | SupportsIndex | None = 0,
                   __end: int | SupportsIndex | None = ...) -> bool:
        "Check if self[start:end][:len(prefix)] == prefix."

    def tostr(self) -> str:
        "Return the internal string representation of self."

    @classmethod
    def fromstrs(cls: type[Self], *iterable: Iterable[str],
                 ignore: bool = False) -> Self:
        "Return a new MoraStr object from multiple strings."

    @staticmethod
    def count_all(__kana_string: str, *, ignore: bool = False) -> int:
        "Return the total number of morae contained in kana_string."


def count_all(__kana_string: str, *, ignore: bool = False) -> int:
    "Return the total number of morae contained in kana_string."


CONVERSION_TABLE: Mapping[str, str]

from . import utils
