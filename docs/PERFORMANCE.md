# 性能と低遅延

## 目標を測定可能にする

240 Hzのframe budgetは約4.17 msです。ただし「240 FPS」と「cursorへ吸い付く低遅延」は同義ではありません。
最低限、次を分けて測ります。

- application CPU: UI宣言、text layout、instance構築
- upload CPU: mapped memory copyとbuffer拡張
- submit/present CPU: command記録、queue submit、driver/compositor待ち
- GPU: render pass内のtimestamp差
- end-to-end: input eventまたは物理pointer位置から実display更新まで

`RendererStats`が直接示すのは最初の4項目です。monitor scanout、OS compositor、mouse polling、camera計測を
含むend-to-end latencyではありません。

## 低遅延frame順序

推奨loop:

```cpp
while (!window.shouldClose()) {
  renderer.prepareFrame(); // 前frame fenceとimage acquireを先に終える
  window.pollEvents();     // edge stateとpointerを更新

  // drag/sliderを宣言する直前に、必要ならabsolute pointerだけ再sample
  window.resampleCursor();

  draw.clear();
  ui.beginFrame(window.input(), draw);
  declareUi();
  ui.endFrame();
  renderer.draw(draw);
}
```

`prepareFrame()`を後段に置くと、最新入力をsampleした後でfence/image待ちが入り、その待ち時間だけ描画位置が
古くなります。現在は1 frame in flightなのでqueueを深くせず、入力と表示の世代差を抑えます。

`pollEvents()`はcallback dispatch後にもcursorをsampleします。`resampleCursor()`はbutton/key edgeを変えず、
absolute positionとdeltaだけを更新するため、sliderやdrag直前に追加できます。すべてのwidget間で何度も呼ぶ
必要はなく、latency-criticalな宣言群の直前に1回で十分です。

## Present mode

| Config | 優先順 | 性質 |
|---|---|---|
| `vsync=true` | FIFO | tear-free、compositor/refresh同期、常に利用可能 |
| `vsync=false`, `allowTearing=false` | MAILBOX → IMMEDIATE → FIFO | 低遅延と滑らかさの標準 |
| `vsync=false`, `allowTearing=true` | IMMEDIATE → MAILBOX → FIFO | latency優先、tearing許容 |

選択結果は`presentModeName()`で必ず確認してください。requestしたmodeがsurfaceで利用できなければfallbackします。
IMMEDIATE時はsurfaceが許す最小swapchain image数を使い、MAILBOXは置換用に1枚追加します。

Windowsのhardware cursorは通常Vulkan contentと別経路でcompositor合成されます。cursor自体が速く見えるのは
正常で、applicationが物理cursorと全く同じscanout時刻を保証することはできません。それでも、古い入力、
余分なframes-in-flight、CPU stall、present queueingを除去すれば、content側の差を最小化できます。

## 動的と保持を選ぶ

### 毎frame動的でよいもの

- 数十〜数百個のcontrol、icon、shape
- slider thumb、hover、selection、短いlabel
- paint、opacity、gradient、clip、destinationが変わるshape

これらは1 shapeあたり1 instanceで、atlas geometryは再生成しません。小さいUIへ複雑なcache invalidationを
入れる方がCPU分岐と状態管理を増やす場合があります。

### 保持すべきもの

- 数千glyphの長文
- 内容/layoutは不変で、zoom/pan/clipだけが毎frame変わるdocument
- 静的な大量background要素

保持APIはtextとDrawListの両方にあります。`createRetainedText()` / `createRetainedDrawList()`はscene準備時に使い、
`update*()`はshared device-local arena内のdirty rangeだけを転送します。同一内容の更新は0-byteです。
zoom/pan frameでは保持geometryをGPU residentのままtransform/scissorだけ変更できます。

`textStatic()`は文字列copyだけを省き、glyph layoutとinstance uploadは省きません。長文性能対策には
`retainedText()`を使います。

### Atlas shapeを切り替える

stroke width、dash、cap、join、taperはatlas構築時のgeometryです。UIから連続変更する試作で毎frame atlasを
buildしてはいけません。頻出候補を事前登録してIDを選ぶか、直線dashのように安価な解析primitiveで表現します。
任意path topologyの編集は別atlasをbackgroundで構築し、安全なscene境界でrendererごと交換する仕事です。

## Allocationと転送

- `DrawList::clear()`はvector capacityを保持する
- rendererのCPU staging vectorもcapacityを保持する
- mapped Vulkan instance bufferは初期capacityから不足時だけ2倍に増やす
- dynamic instanceはhost-visible/coherent memoryへ1回`memcpy`し、staging submitは行わない
- retained text/DrawListは共有device-local arenaへsuballocateし、更新時はdirty contiguous rangeだけcopyする
- interactive frameにGPU→CPU readbackはない

通常frameでbuffer拡張を起こさないよう、`RendererConfig::initialVertexCapacity`をgalleryや最大dynamic glyph数に
合わせます。低メモリ環境では`RendererConfig::lowSpec()`でdynamic capacity、retained arena、GlyphRun cacheを縮小し、
continuous corner処理も簡略化できます。`stats().uploadedBytes`のpeakを観測し、必要なら個別値を上書きしてください。

## Textとclip

動的textはCPUでline/glyphを解決しますが、通常commandではclip外のlineとglyphをinstance化前に除外します。
長いscroll documentを動的textのまま使う場合もclipを狭く保つことが重要です。保持textではhardware scissorを
使い、document全instanceはdevice-localに残します。

完全Unicode shaping engineを後付けする場合も、layout resultをglyph ID/position列としてcacheし、
Slug coverage側へ毎frame shapingを持ち込まない構成を推奨します。

## 計測API

```cpp
slugvk::RendererConfig config;
config.gpuTimingInterval = 16; // 16 submissionごと。0はquery自体を作らない

const auto s = renderer.stats();
```

| Field | 内容 |
|---|---|
| `quads` | dynamic + retainedの描画instance数 |
| `retainedQuads` | そのうち保持buffer由来 |
| `drawCalls` | 宣言順を守るためのbatch数 |
| `uploadedBytes` | 現frameのdynamic instance copy量 |
| `cpuBuildMilliseconds` | DrawListからinstance/batchへ解決した時間 |
| `cpuUploadMilliseconds` | capacity確認とmapped copy時間 |
| `gpuMilliseconds` | samplingしたrender timestamp差の直近値 |
| `cpuSubmitMilliseconds` | command記録、submit、`vkQueuePresentKHR`呼出しまで |

GPU timestampを毎frame書くと計測自体のcommand/query overheadが入ります。常時計測は16〜120 frame程度のintervalを
使い、最終latency測定では0にして比較します。intervalを使う場合、`gpuMilliseconds`はsampling frameの直近値で、
非sampling frameの値ではありません。

`cpuSubmitMilliseconds`にdriverやpresent callの待ちが含まれることがあります。この値が大きくても、vertex/fragment
GPU workが同じだけ重いとは限りません。`gpuMilliseconds`と分離して判断します。

## 再現可能なbenchmark手順

1. OS、GPU、driver、resolution、scale、monitor refresh、compositor設定を記録
2. present modeを`presentModeName()`で記録
3. 同じatlas、quads、glyph数、clip、paintを使う
4. debug/validationを無効にしたReleaseでwarm-up後に測る
5. averageだけでなくp50、p95、p99、最大stallを記録
6. CPU build/upload/submit、GPU、uploaded bytes、draw callsを別々に保存
7. 入力遅延はhigh-speed cameraまたは信頼できるinput-to-photon設備で別測定

開発機RTX 2070 SUPERでの一例では、kitchen-sink/IMMEDIATEの直近観測でCPU build約0.13 ms、upload
約0.01 ms未満、GPU約0.59 msでした。一方、submit/present側は約2.37 msでdriver/DWMの影響が支配的でした。
これは能力保証や他環境との比較値ではなく、「GPU coverageよりpresent経路が律速になり得る」という診断例です。

## 最適化の判断順

1. present modeと本当に選ばれたmodeを確認
2. `prepareFrame()`を入力sample前へ置く
3. drag直前のpointer再sampleを確認
4. dynamic長文をretainedへ移す
5. buffer growth、uploaded bytes、draw call境界を確認
6. GPU timestampでcoverage/overdrawを確認
7. OS compositor/driver/displayのend-to-end測定へ進む

FPSだけを上げるためにbusy loopを増やしたり、UI stateをGPU readbackへ移したりすると、消費電力や遅延が悪化する
ことがあります。最適化はどの区間がbudgetを消費しているかを計測してから行います。
