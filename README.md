# vf_fmdif (Field Match De-interlace Filter)

# これは何？

ffmpeg用のdeinterlace用visual filterです。最も有名なdeinterlacerとも言える[yadif](https://ffmpeg.org/ffmpeg-filters.html#yadif-1)をベースに[fieldmatch](https://ffmpeg.org/ffmpeg-filters.html#fieldmatch) filterの機能を取り込み、元素材が30pや24pの場合の出力クオリティアップを目指しました。

# 詳細

## 仕組み

本filterはyadifとfieldmatchのキメラです。yadifによるdeinterlace処理をかける前にfieldmatchの櫛検出(comb detection)ロジックを使って前後にmatchするfieldがあるかどうかを調べ、もし櫛状にならないfieldが見つかった場合はそちらを採用し、見つからなければyadifによるdeinterlace処理を行う、という動きをします。理論上ソースが30p or 24pであればほぼ情報欠損なしに60i→60p or 30p変換が可能ですが、実際には櫛検出にはheuristicを用いているため本来field match可能な場面でもyadifによる処理になることもあり、そういった場合は多少クオリティが落ちます。

## オプション

利用出来るオプションはyadifのそれとfieldmatchの櫛検出用の一部(cthresh, chroma, blockx, blocky, combpel)です。default値もほとんど同じですが、cthreshとcombpelのみdefaultをそれぞれ10と100に上げています。

## 使い方

ffmpeg-6.1.1のsource codeを展開したあと、そのtop directoryにて `vf_fmdif.patch` ファイルを下記のように適用して通常通りbuildすれば使えます。おそらく他のversionでも問題なく当たるのではないかと。

```
$ patch -p1 -d . < vf_fmdif.patch
```
