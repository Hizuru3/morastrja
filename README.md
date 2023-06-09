# morastrja
*Mora String for the Japanese Language*

仮名文字で表現された日本語のモーラ列を文字列のように扱えるクラスを提供します。
モーラ数のカウントや部分モーラ列の判定を高速に行うことができます。

## License

[MIT License](https://github.com/Hizuru3/morastrja/blob/main/LICENSE)

## Installation

```sh
pip install morastrja
```

ビルドにはCコンパイラ（C99以降）が必要です。

## Usage

```python
# MoraStrクラスをインポート
>>> from morastrja import MoraStr

# インスタンスの作成
>>> MoraStr('モーラ')
MoraStr('モ' 'ー' 'ラ')
>>> MoraStr('シミュレーション')
MoraStr('シ' 'ミュ' 'レ' 'ー' 'ショ' 'ン')

# ひらがなや半角カタカナでもOK
# 自動的に全角カタカナに変換されます
>>> MoraStr('ひらがな')
MoraStr('ヒ' 'ラ' 'ガ' 'ナ')
>>> MoraStr('ﾊﾝｶｸﾓｼﾞ')
MoraStr('ハ' 'ン' 'カ' 'ク' 'モ' 'ジ')

# モーラ数を取得
>>> len(MoraStr('シミュレーション'))
6
>>> MoraStr('シミュレーション').length    # 上と等価
6

# 部分モーラ列判定 (in)
>>> 'シャ' in MoraStr('ショーシャマン')
True
>>> 'ショーシャ' in MoraStr('ショーシャマン')
True
>>> 'シ' in MoraStr('ショーシャマン')
False

# 添え字アクセス
>>> MoraStr('アーティキュレーション')[3]   # 0-indexed で3モーラ目
'キュ'
>>> MoraStr('アーティキュレーション')[-2]  # 後ろから2モーラ目
'ショ'

# スライス
>>> MoraStr('ジェットエンジン')[:3]       # 最初の3モーラを抽出
MoraStr('ジェ' 'ッ' 'ト')

# リスト化
>>> list(MoraStr('シュレッダー'))
['シュ', 'レ', 'ッ', 'ダ', 'ー']

# 文字列型との相互変換
>>> moras = MoraStr('ｺﾝﾋﾟｭｰﾀｰ')
>>> moras
MoraStr('コ' 'ン' 'ピュ' 'ー' 'タ' 'ー')
>>> moras.string
'コンピューター'

```

この他、組み込みの文字列型と共通のメソッド ( .find(), .count()など ) も多くサポートしています。
また、ライブラリにはモーラ列処理に関係するいくつかのユーティリティー関数も含まれています。
詳しくは[ドキュメント](https://hizuru3.github.io/morastrja/)をご覧ください。
