# debugger

[English](README.en.md)

『侍道4』の Mod を作るためのデバッグツールです。いくつかの機能をまとめた Mod で、機能ごとに `config.ini` で使うかどうかを選べます。

| 機能 | 内容 | 既定 |
| --- | --- | --- |
| [エラーログ](#エラーログerror_log)（`error_log`） | 例外を記録し、ゲームが落ちたときはミニダンプも書く | 使う |

どの機能を使っていても、他の Mod は [ノート](#他の-mod-から手がかりを残すノート) で手がかりを残せます。

## エラーログ（error_log）

ゲームの中で例外（アクセス違反など）が起きると、ゲームのフォルダ（`WayOfTheSamurai4.exe` があるフォルダ）の `logs/` に記録します。ゲームを起動してから終えるまでに例外が起きなければ、ファイルも `logs/` フォルダも作りません。

```
logs/error_<日時>_<プロセス番号>.log   … エラーログ（テキスト）
logs/crash_<日時>_<プロセス番号>.dmp   … ミニダンプ（ゲームが落ちたときだけ）
```

エラーログとミニダンプは、同じ回のものなら日時とプロセス番号が同じです。

エラーログの記録は2種類あります。

| 見出し | いつ書くか |
| --- | --- |
| `==== Exception #n (first chance …)` | 例外が起きた時点。ゲームが自分で処理して、そのまま動き続ける例外も含みます。同じ場所の例外は、2回目以降は 2, 4, 8, … 回目のときだけ `---- Exception … again at the same address …: occurrence n` の1行を書きます |
| `==== Fatal exception` | どこでも処理されずに、ゲームが落ちるとき。原因を調べるときは、まずこの節を見てください |

それぞれの記録に書くもの：

- 例外のコードと場所。ゲームの exe の中なら `WayOfTheSamurai4.exe+1234 .text (default VA 00401234)` のように、exe の既定のベースアドレス（`00400000`）に直したアドレスも書きます
- アクセス違反なら、読み書きしようとしたアドレスと、そのメモリの状態（解放済みなら `free (released, or never allocated)` など）
- 例外が起きた命令のバイト列
- 全レジスタと、それぞれが指す先（メモリの状態と先頭 32 バイト。先頭 4 バイトの値が、読み取り専用データを指していれば、仮想関数表かもしれないので `(possibly a vtable)` と書きます）
- スタックの先頭と、スタック上の戻り先らしい値（call の直後を指すもの）。スタックを順に調べて拾うので、関係のない値も混ざります

致命的な例外のときは、さらに次も書きます。

- ノート（他の Mod が残した手がかり。下記）
- 読み込まれているモジュールの一覧
- ミニダンプの結果

エラーログの先頭には、ゲームの exe のファイルサイズと SHA-256 を書きます。どの版の exe の記録かを確かめるのに使えます。

- 例外を握りつぶしたり、ゲームの命令やセーブを書き換えたりはしません。ネットワークへの送信もしません。
- エラーログとミニダンプには、exe と DLL のパスが含まれます。ミニダンプには、さらにゲームのメモリの一部（`full` ではすべて）が含まれます。共有するときは注意してください。

## 設定（config.ini）

`mods/debugger/config.ini` に、機能ごとの節（`[<機能名>]`）があります。`enabled=1` でその機能を使い、`0` で使いません。ファイルや項目がないときは、上の表の「既定」と、下の各項目の既定の値になります。

```ini
[error_log]
enabled=1
minidump=standard
```

`[error_log]` の `minidump` は、落ちたときに書くミニダンプの詳しさです。既定は `standard` です。

| minidump | 中身 | 大きさの目安 |
| --- | --- | --- |
| `off` | 書かない | なし |
| `small` | 全スレッドのスタックとレジスタ、モジュールの一覧 | 数百 KB |
| `standard` | `small` に加えて、グローバル変数、レジスタやスタックが指す先のメモリ、メモリの確保状況 | 数 MB〜数十 MB |
| `full` | プロセスのメモリすべて | 数百 MB〜数 GB |

使えない値を書いたときは既定の値にして、`mod_loader.log` にそのことを書きます。
ミニダンプは WinDbg や Visual Studio で開けます。

## 他の Mod から手がかりを残す（ノート）

他の Mod は、短い文（ノート）を debugger に残せます。debugger は直近の 64 件を覚えておき、ゲームが落ちたときにエラーログへ書きます（エラーログを使わない設定なら、どこにも書きません）。「どの場面の途中で落ちたか」などを知るのに使います。debugger が入っていなければ、何もしません。

[include/wots4/debugger.h](include/wots4/debugger.h) を自分の Mod にコピーして、次のように使います。

```cpp
#include <wots4/debugger.h>

extern "C" BOOL WINAPI Wots4ModInitialize(const wots4::ModContext* context) {
    wots4::debugger::connect(context);  // 初期化の中で一度だけ呼ぶ
    // ...
}

// どこからでも（どのスレッドからでも）。1件 160 バイトまで
wots4::debugger::note("scene change started");
```

ノートはそのままエラーログに書かれるので、英語で書いてください（エラーログは英語だけにしています）。エラーログには、次のように書かれます（`[ ]` の中は、ノートを残した Mod のフォルダ名）。

```
Notes from other Mods (last 64):
  20:14:36.902 thread 12392 [example] scene change started
```

## 入れ方

この Mod を使うには、ModLoader が必要です。

1. [wots4-mods の Releases](https://github.com/nagataaaas/wots4-mods/releases) から、ModLoader（`wots4-modloader-<版>.zip`）をダウンロードします。
2. このリポジトリの [Releases](../../releases) から、`wots4-mod-debugger-<版>.zip` をダウンロードします。
3. 両方の zip の中身を、`WayOfTheSamurai4.exe` と同じフォルダへコピーします。

詳しくは、[入れ方](https://github.com/nagataaaas/wots4-mods/blob/master/docs/INSTALL.md)を見てください。
外すときは、`mods/debugger/` を消します。エラーログとミニダンプが要らなければ、`logs/` の中の `error_*.log` と `crash_*.dmp` も消してください。

## ビルド

必要なもの：Visual Studio 2022 以降（C++ によるデスクトップ開発）、CMake 3.25 以降

[wots4-mods](https://github.com/nagataaaas/wots4-mods)（ModLoader と Mod SDK）が要ります。次のどちらかでビルドします。

- **wots4-mods の中でビルドする：** このリポジトリを `wots4-mods/mods/debugger/` に置き、wots4-mods の README の手順でビルドします。
- **この Mod だけをビルドする：** wots4-mods のパスを `WOTS4_SDK_DIR` に渡します。

```bash
cmake -S . -B build -A Win32 -DWOTS4_SDK_DIR=<wots4-mods のパス>
cmake --build build --config Release
ctest --test-dir build -C Release -LE needs_exe
```
