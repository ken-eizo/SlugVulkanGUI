# ネイティブホストへの埋め込み設計

## 目的と現在地

After Effectsのdockable panelのように、外部applicationが所有するnative UI領域へSlugVulkanGUIを描画する
ことは重要な利用例です。ただし、SlugVulkanGUI本体をAfter Effects、AEGP、Panelator固有仕様にはしません。

現在の所有境界は次の通りです。

- `Window`: GLFW top-level windowとinput callbacks
- `PlatformSurface`: Vulkan surface作成、pixel size/scale、visibility、redrawの抽象契約
- `InputWriter`: native eventからhost非依存`InputState`を作る書込API
- `VulkanRenderer`: Vulkan instance、physical/logical device、queue、surface、swapchain、
  pipeline、atlas GPU resource、frame synchronization

`VulkanRenderer(PlatformSurface&, ...)`に加え、公開境界として`RenderDevice` / `RenderSurface`を実装済みです。
Windowsでは`Win32PlatformSurface`が`HWND`を直接受け取るため、GLFWなしで外部所有Viewへ接続できます。
macOSの`NSView/CAMetalLayer`やAfter Effects等のhost固有adapterは別層です。現時点の`RenderDevice`はatlas/configを
共有する公開facadeで、各`RenderSurface`内部のVkDevice resource共有は今後の最適化余地として残しています。

## 分離するべき所有単位

現在の公開backend境界:

```text
Application / plugin adapter
   | native view, size/scale, input, invalidate
   v
PlatformSurface contract
   |
   +--> RenderDevice  (VectorAtlas/RendererConfig + surface factory boundary)
   |
   +--> RenderSurface (VkSurfaceKHR/swapchain, extent, per-image sync)
   |
   +--> UiContext     (panelごとのhover/focus/active/model)
```

### `RenderDevice`（実装済み公開境界）

- `VectorAtlas`への非所有参照と`RendererConfig`を保持
- `PlatformSurface`またはGLFW `Window`から`RenderSurface`を生成
- renderer実装詳細をapplication/plugin側から隠すstable ownership boundary

`RenderDevice`から複数`RenderSurface`を生成できます。現在は各surface内部の`VulkanRenderer`がVkInstance/VkDevice、
pipeline、atlas GPU resourceを所有するため、device-level GPU resourceの物理共有はまだ行いません。

### `RenderSurface`（実装済み公開境界）

- host native viewから作る`VkSurfaceKHR`
- swapchain images/views/framebuffers
- extent、surface format、present mode
- acquire/present semaphore、fence、frame instance buffer
- resize/out-of-date/minimize処理

panelごとに独立させます。1 panelのresizeや破棄でdevice/atlas全体を再作成しないことが目的です。

### `PlatformSurface` contract（実装済み）

Vulkan instance作成前に必要extensionが分かり、作成後にsurfaceを生成できる契約です。

```cpp
class PlatformSurface {
public:
  virtual std::span<const char* const>
  requiredInstanceExtensions() const noexcept = 0;
  virtual VkResult createVulkanSurface(
      VkInstance, const VkAllocationCallbacks*, VkSurfaceKHR*) const noexcept = 0;
  virtual Vec2 framebufferSize() const noexcept = 0;
  virtual float contentScale() const noexcept = 0;
  virtual bool visible() const noexcept = 0;
  virtual void requestRedraw() noexcept = 0;
  virtual void waitForVisibleFramebuffer() {}
};
```

実装はrendererより長く生存し、返すextension nameの文字列もrenderer構築中は有効に保ちます。
`createVulkanSurface`の成功後、`VkSurfaceKHR`はrendererが所有・破棄します。埋め込みhostはevent loopを
所有しないため、既定の`waitForVisibleFramebuffer()`をblockingさせません。standalone GLFW互換層だけが
最小化解除までeventを待ちます。

## Windows adapter

host SDKが所有するpanel/containerから子`HWND`を取得または作成し、次の一般的な経路を使います。

1. `VK_KHR_surface`と`VK_KHR_win32_surface`をinstance extensionへ追加
2. `VkWin32SurfaceCreateInfoKHR`へ対象`HWND`と`HINSTANCE`を渡してsurface作成
3. `WM_SIZE`またはhost resize通知から最新client pixel sizeを保存
4. resize通知では重い再構築を直接せず、render時にswapchain out-of-date/extentを処理
5. `WM_DPICHANGED`等でlogical→framebuffer scaleを更新
6. mouse/pointer/key/character eventをpanel-local framebuffer座標の`InputState` snapshotへ変換

adapterが作成した子`HWND`を使う場合、そのlifetimeはhost containerより短くします。hostが提供した`HWND`を
借用する場合はdestroyしません。どちらかを型または明示ownership flagで区別し、暗黙判定しません。

`VkSurfaceKHR`とswapchainは`HWND`破棄前に停止・破棄します。現在のframeが実行中なら該当surface fenceを待ち、
全panel共有deviceに対する毎回の`vkDeviceWaitIdle()`は避けます。device loss時はhost processを落とさず、panelを
error stateへ移してhost側へ通知します。

## macOS / MoltenVK adapter

hostが所有する`NSView`内にplugin用child viewを置き、backing layerとして`CAMetalLayer`を用意します。

1. `VK_KHR_surface`、`VK_EXT_metal_surface`、利用可能ならportability enumerationをinstanceへ追加
2. `VkMetalSurfaceCreateInfoEXT::pLayer`へ`CAMetalLayer`を渡してsurface作成
3. viewのbounds、`backingScaleFactor`/content scaleから`drawableSize`をpixelで更新
4. view/layerの作成・付替え・破棄はmacOS main threadで行う
5. resize/scale changeはsurface stateへ通知し、render境界でswapchain再作成
6. MoltenVK deviceがadvertiseした場合だけ`VK_KHR_portability_subset`をenable

host viewのlayer policyをpluginが無断で置き換えず、専用child view/layerを所有する構成を優先します。Retinaの
point座標をそのまま描画座標にせず、常にdrawable pixelへ変換します。

plugin bundleではVulkan loader、MoltenVK dylib、ICD discovery、`@rpath`、code signingを一組で設計します。
host process全体の環境変数やloader search pathを書き換える方式は、他pluginとの衝突を起こすため避けます。

## Input adapter

`InputWriter`は実装済みで、`InputState`の公開read APIを変えずにnative host eventを投入できます。

```text
Host event callbacks -> InputWriter/backend queue -> immutable frame snapshot -> UiContext
```

必要なevent:

- pointer position、relative/raw delta、enter/leave
- 左右middleと追加buttonのpress/release/down
- wheel/trackpad delta、gesture start/active/end
- physical key press/repeat/release
- Unicode committed text
- focus gained/lost、capture lost

capture lost時はdown/activeを必ずcancelし、panel外でbuttonが離されてもstuck dragを残しません。`InputWriter`は
composition text、selection range、commit/cancelをhost非依存`CompositionState`へ渡せます。candidate window位置や
OS text-service連携そのものはhost/native adapter側の責務です。

実装では`focusLost()`が全mouse/key down stateを解除し、active compositionもcancelします。`beginFrame()`、event投入、
`finishFrame(nowSeconds)`をframe境界として使い、`cursor()`へはpanel-local framebuffer pixelを渡します。
core UIは`slugvk::Key`を使用し、GLFW callbackはnative key codeをこのstable enumへ変換します。

低遅延dragでは、event queueを全部処理した後に最新pointer positionを一度sampleし、UI declaration直前にsnapshotへ
反映します。古いmove eventを順番に描画する必要はなく、press/release順だけは失いません。

## Host event loopと再描画

pluginはhostのevent loopを所有しません。adapterは次の2 modeを使い分けます。

- event-driven: state変更、resize、露出、animation tick時に`requestRedraw()`
- active interaction: drag、scroll、zoom、animation中だけdisplay refreshに合わせて連続描画

静止panelで240回/秒busy loopする必要はありません。drag中はhostが許すdisplay-linked callbackまたは短いrender tickを
使い、入力をlate sampleしてからsubmitします。hostのmain threadを占有する独自message loopやsleep loopは作りません。

Windowsのmodal resizeやmacOS live resize中もhostが許可するdraw callbackからdeclarative sceneを再発行します。
swapchain再作成と同時submitを直列化し、0×0/hidden panelではacquireせずinvalidated stateだけ保持します。

## Threading

現在の`Window`/`VulkanRenderer`は単一UI/render thread契約です。埋め込みbackendの第一段階も同じ契約にして、
正しさとhost SDK制約を優先します。

将来render threadを分ける場合:

1. main threadでhost eventとhost SDK APIを処理
2. modelのimmutable snapshotまたはdouble-buffered DrawListをrender threadへ渡す
3. render threadはVulkan resource/submitだけを処理
4. host viewの作成破棄やhost SDK callbackをrender threadから呼ばない
5. panel close時は新規frameを止め、surface fence完了後にresourceを破棄

`UiContext`やmodelをmutexなしで両threadから読む構成は禁止します。高頻度pointerはSPSC queueまたはatomic latest
position、edge eventは順序付きqueueなど、意味の異なる入力を分離します。

## 複数panelとresource共有

1つのhost processで複数panel instanceが存在し得ます。理想構成は次の通りです。

- process/plugin module内にGPUごとの`RenderDevice`
- atlas内容ごとに共有`VectorResources`
- panelごとに`RenderSurface`、dynamic instance ring、`UiContext`、model
- retained documentはdevice resourceとして参照countまたはowner IDで管理

最初のadapterは安全のためpanelごとにdeviceを作る実装でも構いませんが、公開APIにその前提を埋め込みません。
device共有へ移行できるownership境界を先に保ちます。

## ABIとpackage

SlugVulkanGUIをplugin内部へstatic linkする場合、pluginとlibraryを同じcompiler/runtime設定でbuildするのが最も単純です。
host/plugin ABI境界へ`std::string`、`std::vector`、例外、allocator ownershipを直接渡さないでください。

将来binary renderer serviceを分離する場合は、versioned C ABI handle + POD descriptorを検討します。最低限:

- API/structure versionとsize
- caller/calleeのallocation/free対
- exceptionを境界外へ出さないerror code
- device/surface/retained resourceの明示destroy
- thread affinityとcallback lifetime

Adobe等のhost SDKに依存するcode、SDK headers、licensing/build設定は`adapters/<host-name>`または別repositoryへ置き、
`slugvk` targetのpublic dependencyにしません。

## After Effects型panelへの適用

AEGP/Panelator型の利用では、host SDK側がdockable/resizable panelとnative containerを管理し、SlugVulkanGUI adapterが
そのcontainer内の子viewとVulkan surfaceだけを管理する構成にします。

```text
After Effects SDK / panel framework
       | lifecycle, dock, resize, focus, host commands
       v
AE adapter（別層）
       | generic PlatformSurface + InputWriter + redraw callback
       v
SlugVulkanGUI backend
       | DrawList / UiContext / Slug atlas
       v
plugin application model
```

host project data取得、AEGP suite呼出し、undo、render queue、selection等はadapter/application側です。SlugVulkanGUIは
それらを知りません。これにより同じrenderer/UIを他のDCC、editor、standalone toolへ転用できます。

host SDKのversionごとにnative handle取得方法やthread制約が異なる可能性があるため、adapter実装時は対象SDKの
公式仕様を固定し、runtime capability checkを入れます。「Panelatorのような見た目」をコアのwindow ownershipへ
混ぜないことが重要です。

## 実装ロードマップ

### Phase 1: 所有権分離

- 完了: `PlatformSurface`のsurface factory、extent、scale、visibility、redraw contract
- 完了: 既存`VulkanRenderer(Window&, ...)`を互換facadeとして維持
- 未完了: `VulkanRenderer`内部からdevice-levelとsurface-level resourceを共有可能な型へ抽出

### Phase 2: 外部surfaceとinput

- 完了: backend専用`InputWriter`、IME composition、focus loss、frame snapshot test
- 完了: Win32 `HWND` adapter (`Win32PlatformSurface`)
- 未完了: hidden child-windowを使った実機Win32 smoke test
- macOS `NSView/CAMetalLayer` adapterとMoltenVK smoke test
- 具体adapterでのcapture loss、HiDPI、live resize test

### Phase 3: 複数surface

- device/atlas共有、per-surface swapchain/sync
- panel単位destroy、resize storm、minimize、device loss test
- surfaceごとのpresent schedulingとstats

### Phase 4: host adapter

- 対象host SDKを別targetで接続
- dock/undock/live resize/focus/close/reloadを実機検証
- plugin-relative Vulkan/MoltenVK package、codesign、crash-safe teardown
- 240 Hz環境でinput-to-photonとframe pacingを測定

この順序なら、スタンドアロン利用を壊さず、外部hostを特別扱いせず、最終的なAE panel開発を進められます。
