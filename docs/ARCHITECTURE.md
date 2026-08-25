# アーキテクチャ

## 設計目標

SlugVulkanGUIは、動的なUIを毎フレーム宣言し直せる扱いやすさと、Slugの解析的GPUカバレッジを
両立させます。コア方針は次の通りです。

- パスの曲線分解とフォントoutline読込はatlas構築時に一度だけ行う
- フレーム中はgeometryをtessellateせず、小さなquad instanceだけを更新する
- 大きく不変な文書はdevice-local bufferに保持し、zoom/panではtransformだけを変更する
- UIのhit testと状態遷移はCPU上で即座に確定し、GPU readbackを入力経路へ入れない
- Window、Vulkan、UIを分離し、将来の外部ホストadapterがベクターコアを変更せず接続できるようにする

「世界最速」は目標であって現在の保証ではありません。性能は同一コンテンツ、解像度、GPU、present
mode、計測点を固定した再現可能な比較でのみ判断します。

## モジュール

| モジュール | 責務 | 主な所有物 |
|---|---|---|
| `Path` / `VectorAtlas` | path作成、stroke outline化、font読込、Slug atlas構築 | CPU atlasデータ |
| `DrawList` | 1フレームの宣言順、clip、overlay、動的/保持コマンド | CPU command vector |
| `UiContext` | hit test、hover/active/focus、標準widget宣言 | UIの一時状態 |
| `InputState` / `Window` | OS eventをframe単位の入力へ変換 | GLFW window、入力snapshot |
| `VulkanRenderer` | GPU resource、instance解決、submit、present | instance ring、atlas texture、swapchain |
| `Tween<T>` | 時間に基づく任意値の補間 | 小さなCPU状態 |

## データフロー

```text
Path / FreeType outline
        |
        | atlas build時のみ: CPU curve decomposition
        v
slughorn curve + band atlas --------------------+
                                                  |
Input -> UiContext -> DrawList -> instance解決 ---+--> Vulkan vertex/fragment shader
                                                  |       | coverage / paint / clip
Retained text buffer -> transform + scissor ------+       v
                                                       swapchain
```

### Atlas構築時

`VectorAtlas::addPath()`と`addStroke()`はshape IDを割り当てます。strokeのwidth、dash、cap、join、
taperはこの段階でoutline geometryへ反映されます。`loadFont()`はFreeType outlineを同じatlasへ追加し、
`build()`がcurve textureとband textureを確定します。

atlasは`build()`後に不変です。これは制限であると同時に、描画中のgeneration check、再配置、
texture更新、shape単位同期を除去する性能上の契約です。新しいpath topologyが必要な場合は、別atlasを
構築し、それに対応するrendererへ安全な時点で切り替えます。

### フレーム中

`DrawList`はshape ID、destination、clip、paint等を宣言順に保持します。`VulkanRenderer::draw()`は
通常コマンドをhost-visible/coherent instance bufferへ一度コピーし、隣接する動的コマンドを一つの
batchへまとめます。quad頂点は`gl_VertexIndex`から生成されるため、頂点/index geometry bufferは
ありません。

保持テキストは例外です。`createRetainedText()`でglyph instanceを一度解決してdevice-local bufferへ
転送します。その後の`retainedText()`は位置、scale、clipのみをpush constant/scissorで変えます。
宣言順を守るため、動的描画と保持描画の境界ではdraw callが分かれます。

### GPU

vertex shaderはinstanceのdestinationをframebuffer座標へ変換します。fragment shaderはband textureから
候補curveを取り、二次曲線との交差とpixel footprintから解析的coverageを計算し、共通`Paint`を評価して
alpha blendします。fill、stroke、glyphは同じcoverage/paint経路を使用します。

解析的antialiasはSlug shader内で行うため、MSAA、glyph bitmap、SDF cacheは必須ではありません。
ただし、strokeのoutline化やfont outline読込そのものまでGPUが行うという意味ではありません。

## CPUが必要な理由

CPUは次の限定された仕事を担当します。

- Vulkan instance/device/resource/commandの作成とsubmit
- OS inputの受信、hit test、focus、widget state更新
- atlas構築時のFreeType/slughorn処理
- 動的テキストの簡易layoutと、可視instanceの作成
- declarative command順とoverlay順の確定

CPUからGPUへの転送は動的instanceの一方向コピーです。入力結果をGPUからreadbackしてUI状態を決める
経路はありません。hit testやlayoutをcompute shaderへ移すと、同一フレーム中に結果を使うための同期・
readbackが必要になり、通常は遅延を増やします。coverage、paint、transform、clip、blendのように
一方向で完結する処理をGPUへ置くのが、この設計の境界です。

## 動的、静的、保持の意味

| 種類 | 変更できるもの | フレームCPU/転送 | 用途 |
|---|---|---|---|
| Atlas shape | destination、paint、opacity、clip | 1 instance | icon、path、stroke |
| 動的text | text、style、layout、destination | glyph layout + 可視instance | field、label、変化する文章 |
| `textStatic` | 上記と同じ。文字列copyのみ省略 | glyph layout + 可視instance | 寿命が保証された固定literal |
| retained text | position、scale、clip | ほぼcommandのみ、glyph uploadなし | 長文zoom/pan |
| analytic rounded rect | size、各radius、各smoothing、paint | 1 instance | layout panel、control |

`textStatic`の「static」はGPU保持を意味しません。参照文字列を`draw()`完了まで生存させる必要があります。

## 座標系

公開描画座標と`InputState::cursorPosition()`はframebuffer pixelです。HiDPI環境ではGLFWのWindow座標を
framebuffer scaleで変換します。`DrawList::roundedRect()`のradiusもframebuffer pixelの絶対値で、
矩形を配置した後に適用されます。対して`Path::roundedRect()`はatlas内の通常pathなので、destinationで
拡大縮小すればradiusも一緒に変わります。

外部ホストadapterは、hostの論理座標、device pixel ratio、surface extentをこの座標系へ正規化する
責務を持ちます。

## 所有権と寿命

現在の構築順は`VectorAtlas` → `Window` → `VulkanRenderer`です。rendererはatlasとwindowを参照するため、
両者はrendererより長く生存させます。保持テキストIDは作成したrendererだけで有効です。終了時はrendererを
先に破棄するか、明示的に`waitIdle()`してから所有スコープを抜けます。

複数`Window`はプロセス内GLFW参照を共有し、最後の`Window`破棄時だけ`glfwTerminate()`します。GLFWの
platform APIと現在のrendererは同じUI/render threadから直列に扱ってください。親applicationがGLFWを所有する
場合は`WindowConfig::manageGlfwLifetime=false`でprocess-global終了処理を親へ残せます。公開クラスは同時呼出しの
thread safetyを保証しません。

## エラー方針

- programmer error（build後のatlas変更等）は`std::logic_error`
- GLFW/Vulkan resource作成、acquire、submit、present、device loss等は説明付き例外
- `addPath()`等のupstream定義失敗とfont読込失敗は`0`/`false`
- 最小化中の0×0 framebufferは`waitForVisibleFramebuffer()`でeventを待つ
- resize/out-of-date/suboptimal swapchainはrenderer内で再生成する

refresh callback内の例外はcallback境界を越えて投げず保存し、次の`pollEvents()`で再送出します。

## 意図的な非目標

- 完全なUnicode shaping、bidi、locale別line breaking
- 任意SPIR-Vを無制限に注入するcustom pipeline API
- host application固有SDKをコアへlinkすること
- GPU readbackを必要とするUI state machine
- build後atlasを暗黙に可変化すること

これらは拡張層として実装できますが、軽量な通常経路の同期やABIを重くしない契約が先に必要です。
