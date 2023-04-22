# morastrja
Mora String for the Japansese Language

仮名文字で表現された日本語のモーラ列を文字列っぽく扱えます。
モーラ数のカウントや部分モーラ列の判定を高速に行うことができます。

# MoraStrクラスをインポート
>>> from morastr import MoraStr

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
>>> MoraStr('シミュレーション').length    # 上と結果は同じ
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


その他、組み込みの文字列型と共通のメソッドも多くサポートしています。
詳しくはドキュメントをご覧ください。