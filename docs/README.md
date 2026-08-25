# SlugVulkanGUI 技術文書

この文書群は、SlugVulkanGUI を特定製品のプラグインではなく、Windows / macOS 向けの汎用
C++20 ベクター描画・GUIライブラリとして利用するための仕様です。実装済みの契約と将来案を
混同しないよう、将来の設計は明示的に「計画」と記載します。

## 読む順序

1. [ビルドと組み込み](BUILD_AND_INTEGRATION.md) — 要件、CMake、ライフサイクル
2. [アーキテクチャ](ARCHITECTURE.md) — CPU/GPU境界、データフロー、所有権
3. [APIガイド](API_GUIDE.md) — Path、Paint、Text、DrawList、UI、Animation
4. [性能と低遅延](PERFORMANCE.md) — 240 Hz設計、保持/動的描画、計測方法
5. [ネイティブホストへの埋め込み](EMBEDDING.md) — 外部所有View/Windowへ接続するための汎用設計

## 現在のサポート範囲

- `add_subdirectory()` によるソース組み込みと `SlugVulkan::slugvk` CMakeターゲット
- GLFWが所有するトップレベルWindow、Vulkanデバイス、surface、swapchainをまとめて管理する
  スタンドアロン構成
- Slug/slughorn atlas、宣言型`DrawList`、簡易`UiContext`、保持テキスト、入力、アニメーション
- Windows VulkanおよびmacOS MoltenVK

現在の`install`は静的ライブラリと公開ヘッダーを配置する低レベル機能であり、依存ターゲットを
含む完全な`find_package(SlugVulkan)`パッケージではありません。外部ホスト所有の子Window/Viewへ
直接描画するバックエンドも未実装です。どちらも、既存コアを製品固有にせず追加できる境界を
[埋め込み設計](EMBEDDING.md)で定義しています。

## 安定性の区分

- 安定候補: `types.hpp`、`Path`、`VectorAtlas`、`DrawList`、`InputState`、`UiContext`、`Tween`
- バックエンド依存: `Window`、`VulkanRenderer`、`RendererConfig`
- 高度なエスケープハッチ: `Window::native()`、`VectorAtlas::native()`。これらはGLFW/slughorn型を
  公開するため、ABI安定性の対象外です。

バージョンはまだ`0.x`です。破壊的変更はリリースノートで明示し、公開APIを増やす前に、所有権、
スレッド、同期、失敗時の契約をこの文書へ追加します。
