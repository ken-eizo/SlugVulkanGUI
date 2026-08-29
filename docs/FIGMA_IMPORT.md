# FigmaからSlugUIへの取り込み設計

目的は、Figmaの見た目を単なる画像として貼ることではなく、編集可能なSlugUI component、
layout、design token、vector resourceへ変換することです。`tools/figma-slugui`に、現在の
selectionを`.slugui`へ変換するnetwork権限なしの開発pluginを実装しています。出力は
`tools/slugui_compiler.py`が型検査し、runtime parserを使わないC++ headerへAOT変換します。

## 採用する構成

```text
Figma document / selected component
             |
       Figma exporter plugin
             |
          versioned .slugui source
             |
 slugui_compiler.py (parse / type check / SVG path AOT)
             |
 SlugUI IR -> generated C++ descriptor + atlas registration
             |
       SlugUI runtime -> DrawList -> Vulkan
```

製品runtimeはFigma API、parser、network accessを含みません。plugin/compilerは開発時の
toolchainであり、生成物だけをアプリケーションへ組み込みます。これにより編集時の情報量と、
実行時の小ささ・決定性を両立します。

Figma pluginは公式Plugin APIで選択中のcomponent/frameを読み、plugin UIで変換結果と警告を
提示します。実装時は公式の
[FrameNode / auto layout](https://developers.figma.com/docs/plugins/api/FrameNode/)、
[variables](https://developers.figma.com/docs/plugins/working-with-variables/)、
[components](https://developers.figma.com/docs/plugins/api/ComponentNode/)、
[vector network](https://developers.figma.com/docs/plugins/api/VectorNetwork/)、
[styled text segments](https://developers.figma.com/docs/plugins/api/properties/TextNode-getstyledtextsegments/)
を基準にします。

## semantic-first mapping

| Figma | SlugUI / SlugVulkan | fidelity |
|---|---|---|
| Frame auto layout horizontal | `Row` | Native |
| Frame auto layout vertical | `Column` | Native |
| Frame without auto layout | `Absolute`または`Stack` | Native / Approximated |
| padding / item spacing / sizing mode | padding / spacing / auto・fixed・grow | Native |
| Rectangle + independent corner radii | `RoundedRectangleVisual` + `CornerRadii` | Native |
| Rectangle stroke paint / weight / align | `StrokeVisual` + 解析stroke ring | Native |
| Rectangle side-specific stroke weights | `[top, right, bottom, left]` + 非対称解析ring | Native |
| corner smoothing | `CornerSmoothing` 0–100% | Native |
| solid / linear / diamond / radial paint | 共通`Paint` | Native |
| opacity | `Paint::opacity`またはelement opacity拡張 | Native / Approximated |
| vector fill/center stroke geometry | node相対`path-data` -> `Path` -> `VectorAtlas` | BakedVector |
| vector inside/outside stroke geometry | Figma `outlineStroke()`の確定形状をAOT化 | BakedVector |
| text family / numeric weight / italic | `TextVisual` / `TextStyle` + explicit font registry | Native |
| mixed styled text runs | `TextRun[]`（family/size/weight/style/decoration/spacing/line-height/fill） | Native |
| clip content | `clipsChildren` | Native |
| component instance | SlugUI component instance（AOT言語段階） | Planned |
| variant / component property | typed property / enum / boolean | Planned |
| variable / style token | design token property table | Planned |
| blur、shadow、blend mode、mask | 専用effect追加またはasset bake | Approximated / BakedRaster |

色、gradient、text、vectorを可能な限りsemanticに保つのが既定です。表現できないeffectのために
tree全体を画像化せず、そのnodeだけをvector/raster assetへbakeするhybrid方式にします。
任意のplugin data、非表示authoring layer、prototype transitionを無条件にruntimeへ持ち込みません。
Figma documentがDisplay P3の場合はplugin側でlinear-light matrix変換してsRGBへ格納し、rendererは
hexをlinearへ一度だけdecodeします。これによりtext/fill/strokeで同じ色空間契約を共有します。

## 現在のexchange契約

format v3 pluginは選択中のnode群をFigmaのpainter順に
`Absolute selection_root`へまとめて
`.slugui`を直接出力します。各nodeのstable nameにはFigma node IDを含め、さらに次のmetadataを
保持します。

数十nodeを一度に選択でき、同時に選ばれた親の子nodeは二重出力しません。rootのサイズは
選択nodeの`absoluteRenderBounds`の和集合なので、外側strokeやgroup内の描画が
`absoluteBoundingBox`だけで切り落とされません。単一選択も同じwrapper契約を使うため、
drop先はcomponent名や選択数に依存しません。compilerは既存format v1/v2も引き続き受理します。

- format version comment、stable export ID、元のFigma node ID
- node名とcomponent/variant由来
- parent/child順、visible、clip、overlay hint
- layout mode、size mode、constraints、padding、spacing、alignment
- fill、Rectangle stroke paint/辺別width/align、textのrange別
  family/size/numeric weight/italic/underline/strikethrough/letter spacing/line height/fill/text case
- vectorのnode相対fill geometry、outline stroke geometry、paint、厳密な相対配置
- 現在はresolved design token/variable値と明示warning
- import fidelity、warning code、bake理由

SVG path dataはレビュー可能な文字列として`path-data` / `stroke-path-data`へ保持し、compilerが
M/L/H/V/C/S/Q/T/A/Z（相対commandを含む）を厳密にAOT変換します。fill geometryと外側へ広がる
stroke geometryは別ShapeIdでも`path-placement` / `stroke-placement`で元nodeの同一座標系を
保持します。SVG/FigmaのY下向き座標はAOT時にSlugのY上向きへ反転し、表示上の上下を維持します。
Group/Booleanの特殊なrelative transformには`absoluteBoundingBox`を使うため、親原点の二重加算を
行いません。format v1が出したpage座標のvector placementはcompilerが旧形式に限ってlocal boundsへ
再中心化します。v2はFigmaのnode-local geometryとoutline boundsをそのまま保持します。生成componentは
SVGをAOTでC++ path命令へ展開するため、frame描画中にSVG文字列をparseしません。binary imageは
`.slugui`へbase64埋め込みません。将来の大きなassetはcontent hashを
持つ別assetにし、同じhashを重複出力しません。node IDはdiagnosticと
再import差分照合に使いますが、アプリの
永続状態keyは明示したexport IDから生成し、Figma側の複製だけで意図せずstateが共有されない
ようにします。

## fidelityを隠さない

`ImportFidelity`は次の意味で使用します。

- `Native`: SlugUIの意味と描画で再現
- `Approximated`: 編集可能だが一部の見た目/constraintを近似
- `BakedVector`: Slug vector shapeとして保持するが内部構造は編集不可
- `BakedRaster`: 画像assetとして保持
- `Unsupported`: 出力せずerrorまたは明示placeholder

plugin UIはnode別レポートを表示し、AOT compilerはsyntax/type errorをfile/line/column付きで
停止します。将来のstrict fidelity modeでは`Approximated`以下をbuild errorにできます。
「変換成功」と表示しながら黙って見た目やinteractionを落とす実装にはしません。

## 再importと手書きコード

生成ファイルは完全に再生成可能なdirectoryへ分離し、手書きC++を上書きしません。
callbackはstable callback IDまたはgenerated interfaceへ接続し、implementationはユーザー側に
置きます。再importではexport IDを照合し、design更新、追加、削除、fidelity変更を差分表示します。

推奨構成は次のとおりです。

```text
ui/
  source/*.slugui              # Figma出力または人手編集する宣言
  generated/*.slugui.hpp       # AOT生成物
  assets/<content-hash>.*      # bakeされたresource
src/
  ui_actions.cpp               # 手書きcallback/state連携
```

## 実装順

1. 完了: `.slugui` format、parser/type checker、決定的C++ generator
2. 完了: rectangle、text、Row/Column/Absolute、fillとRectangle strokeのFigma exporter
3. 完了: Figma API mock → `.slugui` → AOT compiler integration test
4. 完了: SVG由来vector fill/stroke geometryをSlug `Path`へAOT変換
5. 完了: vector stroke alignmentのexact outline、辺別Rectangle stroke、absolute placement
6. 完了: 多数selection、親子重複除去、render bounds root、range別rich text
7. 次: component/variant/variableをtyped propertyとtokenへ変換
8. 次: strict fidelity mode、incremental reimport、pixel comparison

最初からFigma全機能の完全再現を目指さず、Nativeと判定したsubsetの一致を自動テストで保証し、
subsetを段階的に広げます。

## セキュリティと再現性

- exporterが出した名前やplugin dataをコードとして実行しない
- path traversalを拒否し、asset出力先を生成directory内へ固定
- schema version、plugin version、font/resource hashを生成物へ記録
- fontがない場合は黙って置換せずwarning/errorにする
- remote URLをruntimeから取得しない
- CIでは同じfixtureから同じdescriptor/hashが生成されることを確認

この境界により、Figma連携を追加してもSlugVulkan本体は小さい汎用描画・UIライブラリのままです。

## Exampleでのdrop再読込

source treeからビルドしたExampleのFigma Importページでは、任意の有効なcomponent名を持つ
`.slugui`をウィンドウへdropできます。AOT出力型だけをExample用の固定名へoverrideするため、
単一selectionと複数selectionの両方を扱えます。Exampleだけがfixtureを置換し、現在の
Debug/Release構成を別processでAOT再ビルドしてFigmaページを開き直します。同一bytesのfileは
何も変更せず再起動もしません。変更fileは現在のwindowを閉じる前にcompilerの構文・型検査を通し、
有効な場合だけ一度置換します。library/runtimeへ
`.slugui` parserは追加していません。build失敗時は元fixtureを復元し、詳細を
`build/slugui_drop_rebuild.log`へ保存します。配布アプリで任意UIを動的ロードする契約ではなく、
Figma exporterを反復確認するための開発用導線です。
