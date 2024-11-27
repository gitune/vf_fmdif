# vf_fmdif (Field Match Deinterlacing Filter)

# これは何？

ffmpeg用のdeinterlace用visual filterです。最も有名なdeinterlacerとも言える[yadif](https://ffmpeg.org/ffmpeg-filters.html#yadif-1)をベースに(fmdif2はbwdifベース)[fieldmatch](https://ffmpeg.org/ffmpeg-filters.html#fieldmatch) filterの機能を取り込み、元素材が30pや24pの場合の出力クオリティアップを目指しました。

# 詳細

## 仕組み

本filterはyadif/bwdifとfieldmatchのキメラです。yadif/bwdifによるdeinterlace処理をかける前にfieldmatchの櫛検出(comb detection)ロジックを使って前後にmatchするfieldがあるかどうかを調べ、もし櫛状にならないfieldの組が見つかった場合はそちらを採用し、見つからなければ元のfilterによるdeinterlace処理を行う、という動きをします。理論上ソースが30p or 24pであればほぼ情報欠損なしに60i→60p or 30p変換が可能ですが、実際には櫛検出にはheuristicを用いているため本来field match可能な場面でも元filterによる処理になったり間違ったfield同士を組み合わせてしまうこともあり、そういった場合は多少クオリティが落ちます。

櫛検出の前にfield set(weave frame)を作る時はoriginalのfieldmatch filterとは少し異なり、次のようなlogicとしています。

1. 今のframe(mC)、今のframeの最初のfieldと前のframeの後のfieldを組み合わせたframe(mP)、今のframeの後のfieldと次のframeの最初のfieldを組み合わせたframe(mN)に注目し、frame内の1つ目のfieldの処理時はmPとmC、2つ目のfieldの処理時はmCとｍNを用いる
2. もし前のcycleの同じ組み合わせのframeに櫛がなければそのまま採用
   * 前のcycleがない、または前のcycleでfield match出来なかった時は今のframe(mC)を使う
2. もし2.のframeに櫛があれば、もう一つの組み合わせのframeで櫛検出を行う
3. そちらのframeに櫛がなければ採用、そちらにも櫛があれば通常通りdeinterlace処理を実施

cycle(defaultは5フレーム)毎にmP/mC/mNどの組み合わせを使ったかを覚えておき、同じtiming(cycle内の位置)では同じ組み合わせを使うことでリズムを保とうとするlogicを追加しています。

## オプション

利用出来るオプションはyadif/bwdifのそれとfieldmatchの櫛検出用の一部(cthresh, chroma, blockx, blocky, combpel)です。default値はblockx以外は全て変更し、cthreshは10に、chromaは1、blockyは32、combpelは160としています。また、リズムを保ちたいフレーム数を指定するcycle(defaultは5フレーム)も指定可能です。

## 使い方

ffmpeg-6.1.1/7.0/7.1のsource codeを展開したあと、そのtop directoryにて `vf_fmdif.patch` ファイルを下記のように適用して通常通りbuildすれば使えます。おそらく他のversionでも問題なく当たるのではないかと。

```
$ patch -p1 -d . < vf_fmdif.patch
```

## VMAF値比較

https://bsky.app/profile/digitune.bsky.social/post/3lbwc6kkmbk2p
```
Tsunehisa Kazawa
‪@digitune.bsky.social‬
合ってるかは分からないけれど、ffmpegでvisual filter適用後のVMAF値を取るには下記のような感じでいけそう↓。
$ ffmpeg -i INPUT  -filter_complex "[0:v]split[0v][1v];[0v]yadif=1:-1:1[0vf];[0vf][1v]libvmaf=vmaf_v0.6.1.json:log_path=log.xml:n_threads=8" -an -f null -

で、ffmpegで使えるyadif、bwdifに自作のfmdif、fmdif2(bwdifベースのfmdif)を加えて、
上記の方法でいくつかのサンプルのVMAF値を取ってみた。

yadif, bwdif, fmdif, fmdif2の順で書くと、
sample1: 87.96, 89.65, 90.56, 90.67
sample2: 84.82, 87.37, 88.19, 88.43
sample3: 88.70, 90.28, 91.20, 91.26
sample4: 75.56, 76.59, 76.11, 76.96
1～3まではアニメ、4だけ60iな実写ソースです。

元々fieldmatch filterはVMAF値が高くなりがち、という話を聞きますが、確かにベースと
なったyadif、bwdifより常にfmdif、fmdif2の方が良い値でした。ちょっと嬉しい。

fmdif/fmdif2の良いところは、24p/30p/60iが混在したソースでも何もしなくてもそれなりに
高品質なprogressive化をしてくれるところ。メンテフリー・チューニングフリーでそれなりに
満足出来る絵を出力してくれるのはありがたい。
```
```
