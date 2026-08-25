# ビルドと組み込み

## 要件

- CMake 3.24以上
- C++20 compiler
- Vulkan SDK（headers、loader、`glslc`）
- repositoryのGit submodule: GLFW、FreeType、Slug、slughorn

WindowsはVisual Studio 2022、macOSはClang + MoltenVKを基準に継続的ビルドします。依存版はroot
`README.md`に固定revisionを記載しています。

```bash
git clone --recurse-submodules https://github.com/ken-eizo/SlugVulkanGUI.git
```

通常clone済みの場合:

```bash
git submodule update --init --recursive
```

## CMake options

| Option | top-level default | subproject default | 意味 |
|---|---:|---:|---|
| `SLUGVK_BUILD_EXAMPLE` | ON | OFF | kitchen-sink Exampleを作る |
| `SLUGVK_BUILD_TESTS` | ON | OFF | core testを作る |
| `SLUGVK_ENABLE_VALIDATION` | ON | ON | Debugでvalidationを利用可能にする |
| `SLUGVK_STATIC_MSVC_RUNTIME` | ON | OFF | MSVC static CRTを選ぶ |
| `SLUGVK_INSTALL` | ON | OFF | archiveとpublic headersのraw install rule |

subproject時のdefaultがOFFの項目は、親projectのtarget、CRT、install、testへ不要な影響を与えないための
設定です。必要なoptionだけ`add_subdirectory()`より前に明示してください。

## Windows standalone build

```powershell
$env:VULKAN_SDK = "C:\VulkanSDK\1.x.y"
cmake -S . -B build -DVulkan_ROOT="$env:VULKAN_SDK"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\build\Release\slugvk_example.exe
```

canonical Exampleは`build\Release\slugvk_example.exe`です。shaderはEXEへ埋め込み、top-level MSVC
buildはdefaultでstatic CRTを使うため、隣接shader fileやSlugVulkan DLLは不要です。

短い起動検査:

```powershell
.\build\Release\slugvk_example.exe --smoke
```

tear-free低遅延のMAILBOXを優先してExampleを起動する場合:

```powershell
.\build\Release\slugvk_example.exe --mailbox
```

## macOS / MoltenVK standalone build

LunarG macOS Vulkan SDKの環境を読み込みます。

```bash
source "$HOME/VulkanSDK/1.x.y/setup-env.sh"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DVulkan_ROOT="$VULKAN_SDK"
cmake --build build --parallel
ctest --test-dir build --output-on-failure
open ./build/slugvk_example.app
```

SDK外のMoltenVK packageを使う場合は、実行時にloaderへICD manifestを示します。

```bash
export VK_DRIVER_FILES=/path/to/MoltenVK_icd.json
```

SlugVulkanGUIは、deviceがadvertiseした場合にだけ`VK_KHR_portability_subset`をenableし、instance側では
`VK_KHR_portability_enumeration`と対応flagを組み合わせます。GitHub ActionsのReleaseはUniversal
`arm64+x86_64` appへloader、MoltenVK、bundle-relative ICD manifestを同梱します。

## 既存CMake projectへ組み込む

推奨する現在の配布形式はsource integrationです。

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyApplication LANGUAGES CXX)

# 任意。subproject defaultでもOFFだが、親project側の意図を固定する。
set(SLUGVK_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SLUGVK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SLUGVK_INSTALL OFF CACHE BOOL "" FORCE)
set(SLUGVK_STATIC_MSVC_RUNTIME OFF CACHE BOOL "" FORCE)

add_subdirectory(external/SlugVulkanGUI)

add_executable(my_application main.cpp)
target_link_libraries(my_application PRIVATE SlugVulkan::slugvk)
```

親projectがshared CRT `/MD`を使う場合、`SLUGVK_STATIC_MSVC_RUNTIME=OFF`を維持してください。CRTを跨いで
STL objectやallocation ownershipを渡す構成でruntimeが混在すると不正な解放につながります。

`SlugVulkan::slugvk`は公開include directory、slughorn、FreeTypeをCMake dependencyとして伝播します。
VulkanとGLFWはrenderer実装のprivate dependencyです。ただし`VectorAtlas::native()`を直接使う高度な
コードはslughorn APIに結合するため、upstream更新の影響を受けます。

親applicationが既にGLFWを初期化・終了している場合は、二重のprocess-global lifetime管理を避けます。

```cpp
// 親が先にglfwInit()済み。すべてのSlugVulkan Window破棄後に親がglfwTerminate()する。
slugvk::Window window({.manageGlfwLifetime = false});
```

既定値`true`ではSlugVulkanの`Window`数を参照countし、最初に`glfwInit()`、最後に`glfwTerminate()`します。
どちらのmodeでもGLFW platform操作は同じUI threadへ直列化してください。

## 最小ライフサイクル

```cpp
#include <slugvk/slugvk.hpp>

int main() {
  slugvk::VectorAtlas atlas;
  const auto rectangle = atlas.addPath(slugvk::Path{}.rect(0, 0, 1, 1));
  const auto font = slugvk::findDefaultSystemFont();
  if (!font.empty()) atlas.loadFont(font);
  atlas.build();

  slugvk::Window window({.width = 1280, .height = 800, .title = "My UI"});
  slugvk::VulkanRenderer renderer(window, atlas, {
    .vsync = false,
    .allowTearing = false,
    .initialVertexCapacity = 4096,
    .gpuTimingInterval = 0
  });
  slugvk::DrawList draw;

  while (!window.shouldClose()) {
    renderer.prepareFrame();
    window.pollEvents();

    draw.clear();
    draw.fill(rectangle, {24, 24, 240, 48},
              slugvk::Paint::solid(slugvk::Color::fromRgb8(0x6d5dfc)));
    renderer.draw(draw);
  }

  renderer.waitIdle();
}
```

宣言順の重要点は次の通りです。

1. atlasへshape/fontを登録して`build()`する
2. `Window`を作る
3. atlasとwindowを参照する`VulkanRenderer`を作る
4. 各frameで`prepareFrame()` → `pollEvents()` → declaration → `draw()`
5. rendererをwindow/atlasより先に破棄する

`prepareFrame()`はfence waitとimage acquireを入力sampleより前へ移します。呼ばなくても`draw()`が内部で
実行するため正しく動作しますが、低遅延用途では明示呼出しを推奨します。

## Resize callback

Windowsのmodal resize loop中にも再描画するには、直近のapplication stateから同じ宣言処理を呼べる
callbackを設定します。

```cpp
window.setRefreshCallback([&] {
  renderer.prepareFrame();
  draw.clear();
  declareUi();
  renderer.draw(draw);
});
```

callbackはGLFW event dispatch中に同期実行されます。再帰的な`pollEvents()`、host APIの長時間処理、
同じrendererへの同時submitは行わないでください。例外は`Window`が保存し、次の`pollEvents()`から
再送出します。

## Raw installの境界

```bash
cmake --install build --config Release --prefix /desired/prefix
```

現在のinstall ruleは`slugvk` archiveと`include/slugvk`を配置しますが、GLFW、FreeType、slughornの
export、version file、`SlugVulkanConfig.cmake`を生成しません。したがってinstall tree単独を
`find_package()`する構成は未サポートです。binary packageを導入する際は次を一組で追加する必要があります。

- namespaced exported targetとconfig/version file
- dependency discoveryまたはprivate bundling policy
- Windows CRT/ABI、macOS deployment target、MoltenVK loader policy
- Debug/Release、architecture別artifact

不完全なbinary ABIを固定するより、現在はsource integrationを正規経路とします。

## Release workflow

`v*` tagをpushすると`.github/workflows/release.yml`が以下を行います。

- Windows x64 Releaseのbuild、unit test、SPIR-V validation、単一EXE zip
- macOS Universal Releaseのbuild、unit test、SPIR-V validation、MoltenVK bundle作成
- tagに対応するGitHub Releaseへ両zipをpublish

CIはbuild portabilityを検査しますが、hosted runnerは240 Hz表示遅延の評価環境ではありません。操作遅延は
対象OS、GPU、monitor、compositor上で別途測定します。
