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

### (2025/01/26 追記)

前回のVMAF値比較では、上記の通り「60iのソースとそれをvisual filterでdeinterlace+bob化した60pを比較」していて、厳密に言うと比較している両者のformatが異なるため正確に比較出来ているのか若干疑問がありました。そこで以下のように `tinterlace` filterを使うことで60pのソースをinterlace化→それをすぐdeinterlace+bob化する形とすることで比較対象同士のformatを揃え、より確実に比較できるようにしてみました。

```
$ ffmpeg -i INPUT(60p) -filter_complex "[0:v]split=2[0v][1v];[0v]tinterlace=4,yadif=1:-1:1[0vf];[0vf][1v]libvmaf=vmaf_v0.6.1.json:log_path=log.xml:n_threads=8" -an -f null -
```

[Kodi Samples](https://kodi.wiki/view/Samples)からnative 60p(59.94p)と24p(23.976p)の動画をお借りし、後者を60pへ変換(2-3pulldown)してソースを60pに揃えたうえで上記filter chainにより改めてVMAF値を取ってみると、

|sample|yadif|bwdif|fmdif|fmdif2|
|----|----|----|----|----|
|24p|94.56|96.87|96.29|97.17|
|60p|91.86|95.85|90.70|93.12|

となりました。元が24pで60iへ変換されたsampleは順当にfieldmatch効果でVMAP値が上昇した半面、native 60pのsampleではfmdif/fmdif2のそれぞれ元となったdeinterlace filter、yadif/bwdifよりも結果が悪化しています。これはおそらく櫛検出のミスで、本当はmatchさせてはいけないfield同士をmatchさせてしまいartifactが発生しているcaseがあるから、でしょう。元が60pなソースであることが分かっているのであればどのfieldもmatchさせずにdeinterlace filterへ任せるのが正解ですが、櫛検出を「(interlaceの結果生じた)櫛と思わしきパターンがある」というパターンマッチの強度に頼っているため、櫛の発生が少ない場面では誤ってfield matchさせてしまうことがあるのですね。ちょっと面白かったのは、上記数字はVMAF値の平均(mean)の値ですが、60pソースの場合の最小値(min)を見ると、deinterlace filter 2種(yadif, bwdif)では `61.53, 61.64` に収まっているところ、fieldmatchするfmdif/fmdif2では `55.95` まで悪化している点です。deinterlace filterはあくまでinterlace化で欠けたfieldの隙間を周辺の情報から可能な限り穏当に埋めようとするのに対し、fieldmatchでは「マッチした」と判断したfieldの情報を無条件に持ってきてしまうため誤差がより大きくなってしまうことがある、ということなんでしょうね。そういう意味では「field毎の櫛検出」ではない方法でfield matchする・しないを判断できたらよいのでしょうが、今のところは妙案思いつきません。
