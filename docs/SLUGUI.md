# SlugUI 宣言型IRとruntime

SlugUIは、SlugVulkanの描画器を特定ホストやデザインツールへ結び付けずに、再利用可能な
宣言型UIへするための中間表現（IR）です。C++ builderと、人が読める`.slugui`をbuild時に
型検査してC++へ変換するAOT compilerの両方が同じIRを構築し、軽量runtimeが既存の
`DrawList`へ直接loweringします。製品runtimeにparserや文字列式VMは入りません。

## 実装済みの境界

公開APIは`include/slugvk/slugui.hpp`、実装は`src/slugui.cpp`にあります。

- `Component`: 1つの`Element` tree、型付き`PropertyStore`、callback tableを所有
- `Element`: stable ID、名前、layout、visual、interaction、子要素、import metadataを保持
- `Property<T>` / `ValueSource<T>`: 値または型付きproperty slotを参照
- `PropertyStore::bind`: 明示した依存propertyのrevisionが変化した場合だけ再評価
- layout: `Absolute`、`Row`、`Column`、`Stack`
- size: auto、論理px、物理px、percent、min/max/preferred、grow、padding、spacing、alignment
- visual: 空container、解析的角丸矩形（fill + stroke）、Slug shape、Slug vector text
- state paint: normal、hovered、pressed、disabled
- input: hover enter/exit、press、release、press-edge activation
- overlay: 親のoverlay状態を子へ伝播し、通常commandの後の`DrawList` overlay batchへ出力
- source metadata: provider、document ID、node ID、node name、import fidelity
- `tools/slugui_compiler.py`: comment対応lexer、parser、型検査、決定的C++ header生成、診断
- `tools/figma-slugui`: Figma selectionを`.slugui`へ変換する開発plugin

このruntimeは別のレンダリングパイプラインを作りません。最終出力は従来と同じ
`DrawList`なので、Slug shape、角丸矩形、vector text、paintは既存の1つのVulkan batch設計を
そのまま利用します。

## 最小例

```cpp
namespace sui = slugvk::slugui;

auto root = sui::column(slugvk::hashId("root"));
root.layout.padding = sui::Insets::all(sui::Length::logical(12));
root.layout.spacing = sui::Length::logical(8);

auto button = sui::roundedRectangle(
  slugvk::hashId("button"),
  slugvk::Paint::solid(slugvk::Color::fromRgb8(0x4056a8)),
  slugvk::CornerRadii::all(10),
  slugvk::CornerSmoothing::all(100));
button.layout.height = sui::Length::logical(42);
button.interaction = {.interactive = true, .enabled = true, .callback = 1};
root.add(std::move(button));

sui::Component component(std::move(root));
auto clicks = component.properties().define<std::int64_t>("clicks", 0);
component.on(1, [&](const sui::UiEvent& event) {
  if (event.type == sui::EventType::Activated)
    component.properties().set(clicks, component.properties().get(clicks) + 1);
});

sui::Runtime runtime;
runtime.render(component, window.input(), drawList, viewport, window.contentScale());
```

`Length::logical`は`deviceScale`を1回掛けてframebuffer pxへ解決します。
`Length::physical`は既にframebuffer pxである値に使い、percentは親のinner sizeに対する
0–100の割合です。角丸半径は論理pxとしてscaleした後に`DrawList::roundedRect`へ渡すため、
矩形のwidth/heightで引き伸ばされません。`CornerSmoothing`は0–100%のままです。

## フレームと遅延

`Runtime::render`の順序は次のとおりです。

1. dirty bindingだけを評価
2. 現在のframebuffer viewportでlayout
3. 現フレームのcursor/button edgeでhit test
4. hover/press/activation callbackを同期実行
5. callbackがpropertyを変更した場合、bindingを評価し同じフレームで再layout
6. normal command、overlay commandの順で既存`DrawList`へlowering

Activationをrelease待ちではなくpress edgeで発火するのは、ボタンと直接操作UIの視覚状態を
OS cursorから余分な1フレーム遅らせないためです。releaseも独立したeventとして取得できます。
layoutはCPUで動作しますが、GPU readbackや追加submitは発生しません。文字列名の検索は
component構築時の`find`用で、render時の参照は整数の`PropertyId`です。

`RuntimeStats`はelement数、visible element数、layout pass数、dispatch event数を返します。
高頻度の値変更に対しては、将来のAOT compilerが生成する固定配列とdirty subtreeを追加する
余地を残しつつ、現段階では小さく予測可能なtree walkを優先しています。

## layout規則

- `Row` / `Column`: visible childのintrinsic/explicit sizeを測り、余りを正の`grow`比で配分
- `crossAlignment`: Start、Center、End、Stretch。childの`alignSelf`があれば上書き
- `mainAlignment`: Start、Center、End、SpaceBetween
- `Stack`: width/heightがautoのchildをinner boundsへ広げ、子を同じ領域へ重ねる
- `Absolute`: childのx/yと測定sizeを使用。x/yのautoは0
- `clipsChildren`: 子のclipを親boundsと交差
- rootのauto width/height: 渡されたviewportを満たす
- overlay: layout参加方法は通常要素と同じで、描画/hit-test順だけを手前へ移す

現在のintrinsic text measurementは既存rendererと同じく簡易codepointベースです。完全な
Unicode shaping、bidi、言語別line breakingはSlugUIでは重複実装せず、将来の共通text
layout serviceへ接続します。

## .slugui AOT compiler

```bash
python tools/slugui_compiler.py ui/MyPanel.slugui \
  --output generated/MyPanel.slugui.hpp \
  --namespace my_app::generated
```

`--check`は生成せずparse/type checkだけを行い、`--stdout`はheaderを標準出力へ出します。
出力は一時fileからatomic replaceされ、同じ入力・namespaceから常に同じbytesを生成します。
Exampleの`examples/slugui_demo.slugui`もCMake custom commandでAOT生成され、そのheaderを実際に
C++コンパイルしています。

subproject利用では同じ処理をCMake関数から呼べます。

```cmake
add_subdirectory(external/SlugVulkan)
include(external/SlugVulkan/cmake/SlugUI.cmake)

slugvk_compile_slugui(
  INPUT "${CMAKE_CURRENT_SOURCE_DIR}/ui/MyPanel.slugui"
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/MyPanel.slugui.hpp"
  NAMESPACE my_app::generated)
```

`CLASS_NAME`またはCLIの`--class-name`は生成C++型だけを固定します。入力component名とstable IDは
変えないため、単一selectionとFigmaの複数selectionを同じhost slotへ差し替えられます。

最小grammar:

```text
component MyPanel {
  property int clicks = 0;
  property string label = "Click";

  Column root {
    padding: 12px;
    spacing: 8px;

    Rectangle action {
      height: 42px;
      fill: linear(#4056a8, #6b8cff);
      fill-hovered: #5470c0;
      stroke: linear(#80d8ff, #ff68b0);
      stroke-width: 2;
      stroke-align: inside;
      radius: 10;
      smoothing: 100%;
      callback: activate;

      Text caption {
        text: $label;
        font-size: 15;
        text-align: center;
      }
    }

    Text rich_label {
      text: "Red blue";
      text-align: center;
      text-align-vertical: bottom;
      text-runs: [
        text-run("Red", "Geist", 16, 700, false, true, false, 0, 1.25, #ff0000),
        text-run(" blue", "Geist", 14, 400, true, false, false, 0.5, 1.4, #0000ff)
      ];
    }
  }
}
```

property型は`bool`、`int`、`float`、`string`、`color`、`paint`、`length`、
`radii`、`smoothing`、`border-widths`です。`$name`は同じ型を要求する`ValueSource` attributeへ直接binding
されます。callback名はcomponent内のstable IDへAOT変換され、生成structの
`callback_<name>`としてC++からhandlerを登録できます。

length suffixは`px`（論理px）、`ppx`（framebuffer物理px）、`%`、`auto`です。
paintはcolor literal、`solid`、`linear`、`diamond`、`radial`、`shader`を扱います。
Rectangleの`stroke`にも同じPaintを使用でき、`stroke-width`は論理px、
`stroke-align`は`inside` / `center` / `outside`です。stroke ringは解析シェーダーで
評価され、同一batch内の追加quadになります。`stroke-width: [top, right, bottom, left]`で
4辺を独立指定でき、各辺のInside/Center/Outside量と内外の角丸も個別幅から計算します。
paddingは1値、CSS順の2値`[vertical, horizontal]`、または
`[left, top, right, bottom]`です。radius/smoothingは1値または4角を指定できます。
`text-runs`は静的rangeの文字列と`TextStyle`をAOTで型付き配列へ変換します。引数は
`text, font, size, weight, italic, underline, strikethrough, letter-spacing, line-height, paint`
の順で、各runの文字を連結した結果は`text`と完全一致しなければbuild errorです。
runtimeはrun配列をcopyせず参照し、range別の色・font・weight・size・装飾を同じSlug glyph batchへ
loweringします。

## AOT言語の方針

`.slugui`は人が読む宣言ファイルですが、製品runtimeでparserや文字列式評価をしない方針です。
compilerがbuild時に構文解析、型検査、stable element/callback ID生成、直接property参照の
解決を行い、現在のIRと互換なC++ component/property tableを生成します。

予定する言語要素は次の最小集合です。

- 実装済み: component、typed property、直接property参照
- 実装済み: Row / Column / Stack / Absoluteと共通layout property
- 実装済み: Rectangle / Shape / Text、state paint、callback ID
- 実装済み: ShapeのSVG `path-data` / `stroke-path-data`をSlug atlas assetへAOT変換
- 実装済み: Shapeの`path-placement` / `stroke-placement`によるflatten済み形状の厳密配置
- 実装済み: Figma styled text segmentのrange別`TextRun` AOT変換
- 計画: component instance、slot、条件表示、反復
- 計画: design token/theme、compile-time resource/font/shape参照

任意スクリプトVM、runtime CSS selector、reflection必須のproperty bagは導入しません。
状態とアプリケーション処理は通常のC++へ接続し、言語はUI宣言と型安全なbindingへ集中します。

## 現在の制限

- hot reloadは未実装。compilerはbuild-time AOTのみ
- Figma exporterはRectangleのstroke paint/width/alignと、SVG由来vectorの
  fill/stroke geometry・paintを実装済み。inside/outsideはFigmaのoutline結果を焼き込む。
  EVENODD windingは現在non-zero近似として警告する
- textはrange別family、size、numeric weight、italic、underline/strikethrough、色、
  line height、letter spacing、text caseとblock alignmentを出力し、font fileはアプリ側で明示登録する
- paragraph spacing、paragraph別list nesting、OpenType feature override、hyperlink、
  decorationの個別offset/thickness/color、HarfBuzz shaping/bidiはまだnative再現しない
- Figma APIからgeometryを取得できないvectorは明示placeholderになる。
  variables/component variantsのproperty化は未実装
- conditional/repeater、virtualized list、focus traversal、accessibility treeは未実装
- structural tree mutationをcallback中に行う契約は未定義。property変更を使用する
- style inheritance/theme/token tableは未実装
- dirty subtree layoutは未実装。現在はproperty変更時にtree全体を再layout

これらはIRの型を無制限に増やす前に、Exampleとunit testで必要性と性能を測って追加します。
