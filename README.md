# Dirt Delay v0.5.1

Daisy Versio用パッチ（OD → OD/BC → Delay → 2 kHz LP → OD/BC → Delay → SVF LP →
Tap Boost、Dry/Wet）から始めたSchwungのAudio FX（`audio_fx_api_v2`）。
44.1 kHz / 128 frames / stereo int16、L/R独立処理。

v0.3で、原典のローパス2つ（2 kHz OnePoleとSVF）とTap Boostを外し、Input Gainを追加、
Level Matchを常時Onにして、全パラメータを1ページ8ノブに収めました。
残した部品（DaisySPの`Overdrive` / `Decimator` / `DelayLine`）はC言語で演算単位まで
同じ式に書き直しています（`dsp/dirt_engine.c`）。DaisySP本体への依存はありません。

## Build / test

このSchwungリポジトリのルートで実行。Linux/WSL、Python 3、GCC、
aarch64-linux-gnu-gcc、UI検証用Node.jsが必要です。

```bash
bash scripts/test.sh
bash scripts/build.sh arm64
```

`test.sh`はnativeビルド＋ASan/UBSanのエンジンテスト、実`.so`をdlopenする
ABIスモークテスト、Schwungのページプランナーでの契約検証を行います。
配布物は `dist/dirt-delay-module.tar.gz`。チェーンホストは
`audio_fx/<id>/<id>.so` を読むため、共有ライブラリ名は `dirt-delay.so` です。
ホスト標準9モジュールの配布には加えず、独立パッケージで提供します。

## Install

```bash
python3 scripts/install.py http://move.local:7700 --check
python3 scripts/install.py http://move.local:7700
```

Managerの `POST /modules/install-custom`（tarball upload）を使い、
`modules/audio_fx/dirt-delay/` に展開されます。Managerの/modulesから
Custom installで手動アップロードも可能です。

## ノブ（1ページのみ）

| Knob | 名前 | 範囲 | 内容 | 原典 |
|---|---|---|---|---|
| 1 | Mix | 0〜1 | Dry/Wet（等パワー、音量一定） | KNOB_0 |
| 2 | Drive | 0.3〜1 | ODのドライブ **と** BCのダウンサンプル（原典通り兼用） | KNOB_2 |
| 3 | Time | 0〜250 ms | 2本のディレイ共通（左はその3/4） | KNOB_3 |
| 4 | Feedback | 0〜0.9 | 原典の`min(knob, 0.9)`と同じ範囲 | KNOB_4 |
| 5 | Crush | 0〜1 | BCのビット削減（0〜16bit） | KNOB_1 |
| 6 | Input Gain | 0〜+24 dB | Wet経路の入力ゲイン。歪みの深さを決める | なし |
| 7 | FX 1 | Off / OD / BC | 1段目ディレイの前 | sw[0] |
| 8 | FX 2 | Off / OD / BC | 2つのディレイの間 | sw[1] |

信号の流れ： 入力 × Input Gain → 低音ブースト（固定）→ OD（Drive、常時）→ FX 1 → Delay 1 → FX 2 → Delay 2 →
Level Match → Dryと等パワーでMix。

**Input Gainの効き方**（110 Hzサイン、-12 dBFS入力、Drive 0.3）：0 dBで歪み率約1%、
+24 dBで約33%（v0.4の低音ブースト込み）。Level Matchにより出力音量は変わらず（差0.0 dB）、変わるのは歪みだけ。
Input GainはWet経路にだけかかり、Dryは素通し。

**ステレオ（v0.5、ノブ無し・固定）**：左のディレイ2本をTimeの3/4、右をTimeどおりに
動かします。左右のエコーが別々のタイミングで返るので、モノラルの音源（Moveのシンセの多く）
からでもWetが左右に広がります。ノイズを入れた測定で、Wetの左右の相関は1.0（完全に同じ）から
0.18に下がりました。最初のエコーは右が2×Time、左が1.5×Time。
左右の時間が違うので、Moveの内蔵スピーカーなどモノラルで聴くと、エコーが2回に分かれたり、
少しこもったりして聞こえることがあります。Dryには掛かりません。

**低音ブースト（v0.4、ノブ無し・固定）**：100 Hzを中心に+6 dB持ち上げるローシェルフ
（RBJ biquad、S = 1）を、Wet経路の歪みの**前**に入れています。低音が強めに歪みへ押し込まれ、
ファズにベースブーストを入れたような太い低域になります。55 Hzと880 Hzを同時に入れた測定で、
v0.3では両者が同じだけ出ていたのに対し、v0.4では55 Hzが880 Hzより約+6〜+11 dB前に出ます
（Driveが深いほど差が大きい）。Level MatchがWet全体の音量を揃えるので、全体の音量は変わらず、
低音と中高域のバランスだけが変わります。Dryには掛かりません。

## Level Match（常時On）

Mixを動かしても出力音量が変わりません。

1. **等パワー・クロスフェード**（dry = cos、wet = sin）。原典の直線クロスフェードは
   無相関な2信号だと中央で約3 dB落ちる。
2. **Wetの自動レベル合わせ**。Wet経路の音量はInput Gain・Drive・Feedbackで大きく動くため、
   Wetのパワーを**2×Time前の**Dryのパワー（最初のエコーの元になった区間）に合わせる。
   - 目標値は対称な遅い追従（0.4 s）から計算。補正幅は-36〜+24 dB。
   - 急な増加（Drive・Input Gainを上げる）は、同じブロック内で判定する速い追従で
     **+3 dBに即座に抑える**。ゲインは下げるのが速く、上げるのが遅い（0.3 s）。
   - Dry・Wetの両方に信号があるブロックだけで学習するので、入力が止まった後の
     残響は補正されずにそのまま減衰する。

測定値（`tests/test_engine.c`）：ホワイトノイズで6通りの設定×Mix 5点、減衰音の連打
（Drive 0.3/0.5/0.9）のすべてが±0.66 dB以内。Drive 0.3→1.0、Input Gain 0→+24 dB、
Feedback 0→0.9の急変でも10 ms窓の最大が+3.4 dB。
Time 0付近（Wet≒Dry）ではDryとWetが相関するため、Mix中央でわずかに大きくなります。

## 原典から変えたところ

- **ローパスなし**。原典はDelay 1の出力に2 kHz OnePole、Delay 2の出力にSVF LP
  （Cutoff/Res）を持っていたが、どちらも外した。歪みの倍音がそのまま出る。
  SVFの発散防止ガードもSVFごと不要になった。
- **Tap Boostなし**。
- **Drive 0.3〜1**。DaisySPの`Overdrive`はdrive 0でpre-gainが0になり、入力段ODは常時
  かかっているので、原典はDrive 0でWetが無音だった。0.3でほぼクリーン、0.5で小信号
  ゲイン約24倍（+27 dB）。
- **Time 0 = 1サンプル**。`DelayLine`は遅延0で12000サンプル前（約0.27 s）を読むため、
  ノブを下げ切ると最長ディレイに飛んでいた。
- **ノブの平滑化**。Moveのエンコーダはステップ値なので、Mix/Input Gain/Drive/Feedbackは
  10 ms、Timeは60 ms（小数遅延でテープ風のピッチ変化になりクリックしない）で追従。
- Decimatorのfloat→int変換前クランプと符号なし左シフト（C言語でのUB回避のみ、音は同じ）。
- 1e-15未満の値を0に丸める（デノーマル対策、-300 dB）。
- 出力は±1でハードクリップ。

## 原典どおり残した挙動

- **最初のエコーはTimeの2倍**で返る（ディレイ2本が直列で同じ時間）。Wetには直音が含まれない。
- **FX 2のODはドライブを持たない**。`overdrive_l/_r`をFX 1とFX 2で共有し、FX 2は最後に
  設定された値を使う。FX 1がODのときだけDriveに従い、それ以外は初期値0.5のまま。

## RT安全性

`create_instance`でもmallocしない静的プール（16インスタンス、計約3 MB BSS）。
ファイルI/O・ログ・ロックなし。native実測で1オーディオ秒あたり約4 ms。

状態JSONはversion 3。v0.1（version 1）・v0.2（version 2）の状態も読め、外したキー
（cutoff / res / boost / match）は無視、Input Gainは0 dB、0.3未満のDriveは0.3に
引き上げて復元します。
