# debugger

『侍道4』の Mod を作るための道具です。

## クラッシュの記録

ゲームの中で例外（アクセス違反など）が起きると、その位置とレジスタを、次のファイルに書き出します。

```
mods/debugger/crash_<日時>_<プロセス番号>.log
```

- 記録するのは、最初の32回までです。
- 記録には、ゲームが自分で処理する例外も含まれます。そのため、記録があるだけではクラッシュの原因とは決められません。最後の行が、落ちる直前の例外の手がかりです。
- 例外を握りつぶしたり、ゲームの命令やセーブを書き換えたりはしません。ネットワークへの送信もしません。
- ログには、exe と DLL のパスが含まれます。共有するときは注意してください。

## 入れ方

[wots4-mods](https://github.com/nagataaaas/wots4-mods) の Release に入っています。[入れ方](https://github.com/nagataaaas/wots4-mods/blob/master/docs/INSTALL.md)を見てください。

## ビルド

ふつうは wots4-mods の中でビルドします。この Mod だけをビルドするときは、`-DWOTS4_SDK_DIR=<wots4-mods のパス>` を付けて CMake を実行します。
