# APIガイド

すべての通常APIは`#include <slugvk/slugvk.hpp>`から利用できます。座標、size、clip、corner radiusは
framebuffer pixelです。

## PathとVectorAtlas

```cpp
slugvk::VectorAtlas atlas;

const auto panelShape = atlas.addPath(
  slugvk::Path{}.roundedRect(0, 0, 240, 80, 12, 60));

slugvk::Path curve;
curve.moveTo(0, 30).cubicTo(80, -20, 160, 80, 240, 30);

slugvk::StrokeStyle stroke;
stroke.width = 4;
stroke.join = slugvk::LineJoin::Round;
stroke.cap = slugvk::LineCap::Butt;
stroke.dashLengths = {12, 7};
const auto curveShape = atlas.addStroke(curve, stroke);

atlas.build();
```

`Path`は`moveTo`、`lineTo`、quadratic/cubic Bézier、rect、rounded rect、circle、ellipse、polygon、
starを連結可能です。`addPath()`と`addStroke()`の戻り値`0`は登録失敗です。`build()`後のshape/font追加は
`std::logic_error`になります。

### Strokeの既定値と明示override

通常は`StrokeStyle::cap`だけ指定すれば、pathと全dashの両端へ同じcapを使います。必要な場合だけ
`std::optional` fieldsを指定します。

```cpp
slugvk::StrokeStyle s;
s.cap = slugvk::LineCap::Butt;              // 通常の全端点
s.startCap = slugvk::LineCap::Round;        // path全体の開始だけ
s.endCap = slugvk::LineCap::Square;         // path全体の終了だけ
s.dashStartCap = slugvk::LineCap::Square;   // 各dash開始
s.dashEndCap = slugvk::LineCap::Butt;       // 各dash終了
s.dashCaps.resize(3);
s.dashCaps[2].end = slugvk::LineCap::Round; // 3本目だけ
```

優先順位は「`dashCaps[index]` → path端に一致する`startCap/endCap` → dash共通override → `cap`」です。
明示しないfieldはmemoryや宣言を要求せず、従来のcompactな挙動を保ちます。

`width`、dash array、offset、cap、join、start/end taperはatlas geometryです。shapeを登録した後に
`DrawList::stroke(shape, ..., style)`へ別の`width`等を渡してもgeometryは変わらず、`style.paint`だけが
使われます。動的に異なるwidthやdashを選ぶ場合は候補shapeをatlasへ事前登録し、shape IDを切り替えます。

## Paint

`Paint`はfill、stroke、text、analytic rounded rectで共通です。

```cpp
const auto solid = slugvk::Paint::solid(
  slugvk::Color::fromRgb8(0x36d399), 0.8f);

const auto linear = slugvk::Paint::gradient(
  slugvk::GradientKind::Linear,
  slugvk::Color::fromRgb8(0x6d5dfc),
  slugvk::Color::fromRgb8(0x28c7fa),
  {0, 0}, {1, 1}, 0.95f);

const auto procedural = slugvk::Paint::shader(
  slugvk::Color::fromRgb8(0xff4d8d),
  slugvk::Color::fromRgb8(0xffd166), 0.6f);
```

`GradientKind`はSolid、Linear、Diamond、Radial、Shaderです。origin/targetはshape内の正規化paint座標です。
`opacity`はColor alphaに加えてpaint全体へ適用されます。現在の`Shader`は内蔵procedural分岐とfloat
parameterであり、利用者の任意SPIR-V moduleを登録するAPIではありません。

## DrawList

```cpp
draw.clear();
draw.setClip({0, 0, framebuffer.x, framebuffer.y});
draw.fill(panelShape, {20, 20, 320, 100}, linear);
draw.stroke(curveShape, {20, 150, 320, 80}, procedural);

draw.beginOverlay();
draw.roundedRect({300, 60, 180, 120}, 12, solid, 100);
draw.endOverlay();
```

`DrawList`は宣言順を保ちます。通常commandの後に全overlay commandを出すため、dropdownやtooltipを
panelより前面にできます。`clear()`はcommandsとoverlay modeをresetしますが、vectorのcapacityは再利用します。

### 絶対pixelの角丸

```cpp
draw.roundedRect({20, 20, 360, 72}, 12, paint, 80);

draw.roundedRect(
  {20, 110, 360, 72},
  {.topLeft = 4, .topRight = 12, .bottomRight = 20, .bottomLeft = 28},
  paint,
  {.topLeftPercent = 0, .topRightPercent = 25,
   .bottomRightPercent = 50, .bottomLeftPercent = 100});
```

矩形のwidth/heightを確定した後に各radiusを適用します。互いのradiusが物理的に収まらない場合だけ、
CSS互換の比率で全radiusを縮小します。通常の範囲では指定pixel値を維持します。

## Vector text

```cpp
const auto fontPath = slugvk::findDefaultSystemFont();
const auto needed = slugvk::decodeUtf8("日本語ABC");
atlas.loadFont(fontPath, needed);

slugvk::TextStyle style;
style.fontName = "system-ui";
style.size = 18;
style.bold = true;
style.italic = false;
style.underline = true;
style.lineHeight = 1.4f;
style.letterSpacing = 0.5f;
style.indent = 12;
style.align = slugvk::HorizontalAlign::Left;
style.listMarker = slugvk::ListMarker::Bullet;
style.paint = paint;

draw.text("Vector text", {20, 20, 480, 120}, style);
```

空のcodepoint listで`loadFont()`するとASCII 95 glyphを読みます。日本語等は必要codepointを明示して
atlasへ含めてください。UTF-8 decoderは不正continuation、overlong、surrogate、Unicode範囲外をU+FFFDへ
置換します。

現在のlayoutは改行単位のcodepoint layoutです。Left/Center/Right、line height、letter spacing、indent、
簡易bullet/number marker、synthetic bold/italic、underline/strikethroughを扱います。`Justify` enumは将来用で、
現時点では語間展開を行いません。HarfBuzz shaping、bidi、IME composition、selection、locale line breakは
別のtext layout/editor層が必要です。

### 3種類のtext宣言

- `text(std::string, ...)`: commandが文字列を所有。毎フレーム変化するlabel向け
- `textStatic(std::string_view, ...)`: copyを省くが、参照bytesを`draw()`終了まで生存させる
- `createRetainedText()` + `retainedText()`: layout/uploadを一度だけ行い、zoom/panはtransformのみ

```cpp
const auto document = renderer.createRetainedText(longText, {0, 0, 900, 5000}, style);

draw.setClip(viewport);
draw.retainedText(document, pan, zoom);
```

保持IDは作成元rendererの寿命内だけ有効で、個別削除・更新APIはまだありません。

## Input

`Window::pollEvents()`ごとにedge stateを更新します。

```cpp
const auto& input = window.input();
const auto cursor = input.cursorPosition();
const auto delta = input.cursorDelta();

if (input.mouse(slugvk::MouseButton::Left).pressed) { /* 押したframe */ }
if (input.mouse(slugvk::MouseButton::Left).down)    { /* 保持中 */ }
if (input.mouse(slugvk::MouseButton::Left).released){ /* 離したframe */ }

if (input.scroll().started) { /* gesture開始 */ }
if (input.scroll().active)  { /* 最後のeventから120 ms以内 */ }
if (input.scroll().ended)   { /* timeoutしたframe */ }
```

keyboard codeは現在GLFW key codeです。Unicode文字入力は`textInput()`、物理key状態は`key(code)`を使い分けます。
raw mouse motionはplatformが対応する場合に`setRawMouseMotion(true)`でcursor captureと共に有効化します。

## UiContext

`UiContext`はimmediate/declarative UIです。modelはapplicationが所有します。

```cpp
slugvk::UiSkin skin;
skin.rectangle = rectangleShape;
skin.circle = circleShape;
skin.check = checkShape;
skin.cornerRadius = 8;
skin.continuousCornersPercent = 100;
slugvk::UiContext ui(skin);

ui.beginFrame(window.input(), draw);
if (ui.button(slugvk::hashId("save"), "Save", {20, 20, 120, 34})) save();
ui.slider(slugvk::hashId("opacity"), "Opacity", {20, 64, 260, 34}, opacity, 0, 1);
ui.textField(slugvk::hashId("name"), {20, 108, 260, 34}, name, "Name");
ui.endFrame();
```

各IDは同一frame内で一意かつframe間で安定させます。labelをIDとして暗黙利用しません。同じ表示文字列の
buttonも別IDなら共存できます。treeの内部IDは親とsibling indexから作るため同名nodeが衝突しません。

widgetの`bool`戻り値は、button/radioではpress edge、model参照を受けるcontrolでは値が変化したframeです。
視覚反応もpress frameで宣言されます。`interaction()`を使えばhovered、pressed、released、held、cursor、
deltaをcustom widgetで取得できます。

実装済みwidget:

- button、spin button、slider、list box、checkbox、combo/dropdown、radio
- UTF-8 text field、toggle switch、縦横scrollbar、tooltip
- tree view、grid view

combo/dropdownとtooltipはoverlay commandへ送られます。同時に開けるcombo ownerは一つで、menu領域は下の
通常widgetのhit testを遮断します。

## Animation

```cpp
auto opacityTween = slugvk::tween(0.0f, 1.0f, 180.0f, slugvk::Easing::EaseOut);
const float opacity = opacityTween.update(deltaMilliseconds);
```

組み込み補間は`float`と`Color`です。任意型は`Tween<T>`へinterpolatorを渡します。時間単位はmsで、
Linear、EaseIn、EaseOut、EaseInOut、SmoothStep、Springを選べます。animation自体はframe schedulerを
所有しないため、host loopが`deltaMs`を供給し、必要な間だけ再描画します。
