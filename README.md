# vf_fmdif (Field Match De-interlace Filter)

# これは何？

ffmpeg用のdeinterlace用visual filterです。最も有名なdeinterlacerとも言える[yadif](https://ffmpeg.org/ffmpeg-filters.html#yadif-1)をベースに[fieldmatch](https://ffmpeg.org/ffmpeg-filters.html#fieldmatch) filterの機能を取り込み、元素材が30pや24pの場合の出力クオリティアップを目指しました。

# 詳細

## 仕組み

本filterはyadifとfieldmatchのキメラです。yadifによるdeinterlace処理をかける前にfieldmatchの櫛検出(comb detection)ロジックを使って前後にmatchするfieldがあるかどうかを調べ、もし櫛状にならないfieldの組が見つかった場合はそちらを採用し、見つからなければyadifによるdeinterlace処理を行う、という動きをします。理論上ソースが30p or 24pであればほぼ情報欠損なしに60i→60p or 30p変換が可能ですが、実際には櫛検出にはheuristicを用いているため本来field match可能な場面でもyadifによる処理になったり間違ったfield同士を組み合わせてしまうこともあり、そういった場合は多少クオリティが落ちます。

櫛検出の前にfield set(weave frame)を作る時はoriginalのfieldmatch filterとは少し異なり、次のようなlogicとしています。

1. 今のframe(mC)、今のframeの最初のfieldと前のframeの後のfieldを組み合わせたframe(mP)、今のframeの後のfieldと次のframeの最初のfieldを組み合わせたframe(mN)に注目し、frame内の1つ目のfieldの処理時はmPとmC、2つ目のfieldの処理時はmCとｍNを用いる
2. もし前のcycleの同じ組み合わせのframeに櫛がなければそのまま採用
   * 前のcycleがない、または前のcycleでfield match出来なかった時は今のframe(mC)を使う
2. もし2.のframeに櫛があれば、もう一つの組み合わせのframeで櫛検出を行う
3. そちらのframeに櫛がなければ採用、そちらにも櫛があれば通常通りdeinterlace処理を実施

cycle(defaultは5フレーム)毎にmP/mC/mNどの組み合わせを使ったかを覚えておき、同じtiming(cycle内の位置)では同じ組み合わせを使うことでリズムを保とうとするlogicを追加しています。

## オプション

利用出来るオプションはyadifのそれとfieldmatchの櫛検出用の一部(cthresh, chroma, blockx, blocky, combpel)です。default値もほとんど同じですが、cthreshとchromaのみdefaultをそれぞれ10と1に変更しています。また、リズムを保ちたいフレーム数を指定するcycle(defaultは5フレーム)も指定可能です。

## 使い方

ffmpeg-6.1.1のsource codeを展開したあと、そのtop directoryにて `vf_fmdif.patch` ファイルを下記のように適用して通常通りbuildすれば使えます。おそらく他のversionでも問題なく当たるのではないかと。

```
$ patch -p1 -d . < vf_fmdif.patch
```
