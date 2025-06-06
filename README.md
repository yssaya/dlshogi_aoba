# DeepLearningShogi(dlshogi)
[![pypi](https://img.shields.io/pypi/v/dlshogi.svg)](https://pypi.python.org/pypi/dlshogi)

これは[dlshogi](https://github.com/TadaoYamaoka/DeepLearningShogi)の2025年4月3日版を元に
- NNの入力の特徴に手数、手番を追加。
- 歩の枚数を8枚Maxでなく18枚までに。
- 手数は190手以上から40手ごとに区切って0(190以下)から8(470以上)の9種類に分ける。
  (小数点にしたかったがdlshogiは高速化のため0、1のONE HOT専用になってるため)
- GPUの最大数を8から18に。
- 定跡用に aobabook.cpp を追加。
という変更を加えたものです。

2025年選手権のAobaZeroは、これに384x30bの重みを使ったもので参加しています。
大会で使った重みはReleaseで公開しています。

[選手権版AobaZeroの詳細](http://www.yss-aya.com/bbs/patio.cgi?read=174&ukey=0)

以下はオリジナルの説明です。

--------------------------------------------------------------------------------

将棋でディープラーニングの実験をするためのプロジェクトです。

基本的にAlphaGo/AlphaZeroの手法を参考に実装していく方針です。

検討経緯、実験結果などは、随時こちらのブログに掲載していきます。

http://tadaoyamaoka.hatenablog.com/

## ダウンロード
[Releases](https://github.com/TadaoYamaoka/DeepLearningShogi/releases)からダウンロードできます。

最新のモデルファイルは、[棋神アナリティクス](https://kishin-analytics.heroz.jp/lp/)でご利用いただけます。

## ソース構成
|フォルダ|説明|
|:---|:---|
|cppshogi|Aperyを流用した将棋ライブラリ（盤面管理、指し手生成）、入力特徴量作成|
|dlshogi|ニューラルネットワークの学習（Python）|
|dlshogi/utils|ツール類|
|selfplay|MCTSによる自己対局|
|test|テストコード|
|usi|対局用USIエンジン|
|usi_onnxruntime|OnnxRuntime版ビルド用プロジェクト|

## ビルド環境
### USIエンジン、自己対局プログラム
#### Windowsの場合
* Windows 11 64bit
* Visual Studio 2022
#### Linuxの場合
* Ubuntu 18.04 LTS / 20.04 LTS
* g++
#### Windows、Linux共通
* CUDA 12.1
* cuDNN 8.9
* TensorRT 8.6

※CUDA 10.0以上であれば変更可

### 学習部
上記USIエンジンのビルド環境に加えて以下が必要
* [Pytorch](https://pytorch.org/) 1.6以上
* Python 3.7以上 ([Anaconda](https://www.continuum.io/downloads))
* CUDA (PyTorchに対応したバージョン)
* cuDNN (CUDAに対応したバージョン)

## 謝辞
* 将棋の局面管理、合法手生成に、[Apery](https://github.com/HiraokaTakuya/apery)のソースコードを使用しています。
* モンテカルロ木探索の実装は囲碁プログラムの[Ray+Rn](https://github.com/zakki/Ray)の実装を参考にしています。
* 探索部の一部にLeela Chess Zeroのソースコードを流用しています。
* 王手生成などに、[やねうら王](https://github.com/yaneurao/YaneuraOu)のソースコードを流用しています。

## ライセンス
ライセンスはGPL3ライセンスとします。
