# mi_penbridge

将任意第三方手写笔按键桥接进澎湃（HyperOS/MIUI）手写笔框架的零侵入守护进程
以联想 Tab Pen Plus（Lenovo Tab Pen Plus）为例移植到小米 K Pad 2 (yili) HyperOS 4 移植系统，无需修改任何框架 JAR / APK / system 分区文件

---

## 实现思路

澎湃框架对"手写笔"的全部判定收口在一个静态白名单函数上：

```java
// android.view.InputDevice (framework.jar)
public static int isXiaomiStylus(int vendorId, int productId)
```

所有功能入口——设置页识别（SecurityCoreAdd 的 ViewModel）、按键快捷功能（`MiuiStylusShortcutManager`）、防误触联动（`MiuiEventBlockerManager`）、输入模式切换（`MiuiStylusDeviceListener` → `ITouchFeature.setTouchMode`）——都只查 `device.isXiaomiStylus() > 0`，**不校验调用者身份、Binder 来源、设备指纹，也没有 native 层独立白名单**（`libmiinputflinger.so` 的 `NativeMiInputManager::isXiaomiStylus` 只是转发回 Java）

因此为了避免对框架和apk的修改，决定: **不动框架，伪造手写笔的身份**

1. **身份伪装**：通过 `/dev/uinput` 常驻注册一台 VID/PID 落在小米白名单内的虚拟笔（本项目选 `0x1915:0x4D81` → 版本 2，Redmi Smart Pen），设备名同时取 "Redmi Smart Pen" 以通过设置页基于设备名的类型检测，虚拟设备存活期 = 进程存活期，设置页因此持续显示"已连接"
2. **按键翻译**：监听实体笔的 BLE HID 节点（`/dev/input/eventN`，按 vendor/product + 接口名自动定位），把固件上报的点击模式翻译成框架认识的键值
3. **语义合成**：框架的快捷功能只在**按键持续 ≥380ms** 时触发（写死的 `mHandler.sendEmptyMessageDelayed(..., 380L)`）,所以 penbridge 不透传原始按下/抬起，而是"代笔"合成一次足够长的按住：
   - 单击（固件 `MSC_SCAN 0x0c0600`）→ 合成 `KEY_PAGEDOWN` 按住 3000ms → 框架 380ms 定时器触发速记遮罩 → **用户用笔尖在遮罩上书写**（遮罩触摸监听器校验 `toolType == TOOL_TYPE_STYLUS`）→ 落笔即触发 `launchNote()` 打开便签并带上手写内容；不书写则 3 秒后合成抬起，遮罩自动取消
   - 双击（固件 `MSC_SCAN 0x0c0601`）→ 合成 `KEY_PAGEUP` 按住 450ms → 触发区域截屏
   - 联想笔固件本身就区分单击/双击上报码，无需软件做双击判定

框架侧原生支持的键位语义（`Generic.kl` 已内置映射，无需改 keylayout）：

---

| 合成键值 | 框架判定 | 功能 | 交互 |
|---|---|---|---|
| `KEY_PAGEDOWN` (109 → 93) | `isQuickNoteKey` | 灵感速记 | ≥380ms 弹遮罩，笔尖书写触发 |
| `KEY_PAGEUP` (104 → 92) | `isScreenShotKey` | 区域截屏 | ≥380ms 立即截屏 |

---

## 主要函数说明

| 函数 | 职责 |
|---|---|
| `main` | 参数解析 → 安装信号处理 → `create_virtual_pen` 建常驻虚拟设备 → 初始化 `/dev/input` 的 inotify → 主循环 `poll` 三路事件（inotify / 实体笔节点），处理设备热插拔与重连 |
| `create_virtual_pen` | 打开 `/dev/uinput`，声明 EV_KEY/EV_SYN/EV_MSC 能力位（`KEY_PAGEDOWN`、`KEY_PAGEUP`、`BTN_STYLUS`、`BTN_STYLUS2`、`MSC_SCAN`），以 `UI_DEV_SETUP` 写入伪装身份（`BUS_BLUETOOTH` + 0x1915:0x4D81 + 名称），`UI_DEV_CREATE` 上线 |
| `find_and_open_pen` | 遍历 `/dev/input/event*`，`EVIOCGID` + `EVIOCGNAME` 匹配实体笔的 Consumer Control 接口（vendor 0x17EF / product 0x617F 且名称含 "Consumer Control"），返回只读 fd,设备节点号会因蓝牙重连而变化，匹配条件与节点号解耦 |
| `forward_events` | 核心翻译循环,读 `struct input_event` 批次，维护 `last_scan` 状态：`MSC_SCAN` 更新最近扫描码，`EV_SYN` 复位；`KEY_UNKNOWN(240)` **按下沿**（value==1）到达时按 `last_scan` 分派——`0x0c0600` → `synth_press(KEY_PAGEDOWN, 3000)`，`0x0c0601` → `synth_press(KEY_PAGEUP, 450)`,原始 down/up 事件被吞掉，只输出合成事件,读失败（ENODEV 等）返回 -1 触发上层重扫 |
| `synth_press` | 合成一次按键：DOWN → SYN → 按住 `hold_ms` → UP → SYN,按住时长必须大于框架 380ms 触发线；速记用 3000ms 以便用户在遮罩上书写，截屏用 450ms（截屏在 380ms 即刻触发，无需久按） |
| `emit` | 向 uinput fd 写单条 `input_event`（错误进 logcat） |

```
aarch64-linux-android28-clang -O2 -Wall -Wextra -o penbridge penbridge.c -llog
llvm-strip penbridge
```

---

## 澎湃框架内可选的伪装对象

`InputDevice.isXiaomiStylus(vendorId, productId)` 完整白名单（HyperOS 4.0 / yili，反编译自 framework.jar classes4.dex）,伪装对象的选择决定框架启用的功能面：

| 版本 | vendor | product | 已知设备名 | 功能面 | 适用性 |
|---|---|---|---|---|---|
| 1 | 0x1915 (6421) | 0xEAEA (60138) | Xiaomi Focus Pen | 基础：设置页 + 92/93 快捷键 | 可用 |
| **2** | **0x1915 (6421)** | **0x4D81 (19841)** | Redmi Smart Pen（另有名称集 "REDMI Smart Pen"/"REDMI Smart Pen 1S"/"POCO Smart Pen 1S"/"Xiaomi Active Pen"/"Xiaomi Active Pen (EU)"）| **基础功能，无扩展路径，最干净（本项目采用）** | **推荐** |
| 3 | 0x22 (34) | 0x4D80 (19840) / 0x3280 (12928) | POCO Focus Pen | 基础 + 短视频遥控（`isXiaomiStylus() >= 3` 时 92/93 被遥控逻辑拦截） | 需要遥控才选 |
| 4 | 0x22 (34) | 0x4E73 (20099) | 未知 | 基础 + 遥控 | 可用 |
| 5 | 0x22 (34) | 0x3183 (12675) | 未知 | 基础 + 遥控 | 可用 |
| 6 | 0x22 (34) | 0x3283 (12931) | 未知 | 基础 + 遥控 | 可用 |
| 7 | 0x22 (34) | 0x4F84 (20356) | 未知 | 基础 + 遥控 | 可用 |
| 8 | 0x22 (34) | 0x5081 (20609) / 0x3281 (12929) | P81C 触控膜笔（`TOUCH_FILM_STYLUS = {8}`）| 基础 + 遥控 + 触摸膜按键（196/197 → 合成 93/92）+ 虚拟激光 | 触控膜场景专用 |
| 9 | 0x22 (34) | 0x5183 (20867) / 0x5184 (20868) | 未知 | 基础 + 遥控 | 可用 |
| 10 | 0x22 (34) | 0x5185 (20869) | 未知 | 基础 + 遥控 | 可用 |
| 11 | 0x22 (34) | 0x5186 (20870) | 未知 | 基础 + 遥控 | 可用 |

选型建议：

- **只要按键快捷功能选版本 2**：`isXiaomiStylus() >= 3` 才启用的短视频遥控（foucs 窗口滑动翻页）和版本 8 的触摸膜/激光路径都会改变 92/93 键的处理逻辑，版本 2 让行为最可预测
- **版本号同时影响设置页**：设置页另有一套按**设备名**的类型检测（`C.a()`，返回 1/2/3/4），与 VID/PID 无关,虚拟设备名建议与所选版本对应的官方名称一致（版本 2 对应上表名称集之一），否则设置页会按 UNKNOWN 类型渲染 UI（yili 上会出现激光选项错位等外观问题）
- 息屏速记（`SCENE_OFF_SCREEN`）依赖实体笔通道的 MotionEvent（`processMotionEventForQuickNote`，要求 `SOURCE_STYLUS` + `toolType==pen`），虚拟 uinput 键盘设备**不参与**该路径

---

## 伪装对象与被伪装对象的函数说明

面向要把其他品牌笔接入澎湃框架的开发者,两侧各自的"函数契约"如下——只要你的桥接器能让左侧（设备侧）满足右侧（框架侧）的每一项检查，快捷功能即可复用

---

### 框架侧（被伪装对象：澎湃框架的判定函数）

| 框架函数/判定 | 所在 | 检查内容 | 桥接器需满足 |
|---|---|---|---|
| `InputDevice.isXiaomiStylus(v,p)` | framework.jar | 输入设备的 VID/PID 白名单 | uinput 设备 ID 落在上表内 |
| `MiuiStylusShortcutManager.isQuickNoteKey(ev)` | miui-services.jar | `keyCode == 93 && device.isXiaomiStylus() > 0` | 合成 `KEY_PAGEDOWN`（Linux 109，Generic.kl 已映射）|
| `MiuiStylusShortcutManager.isScreenShotKey(ev)` | miui-services.jar | `keyCode == 92 && device.isXiaomiStylus() > 0` | 合成 `KEY_PAGEUP`（Linux 104）|
| `interceptQuickNoteKey` → 380ms 定时器 | miui-services.jar | DOWN 后 380ms 内未抬起 → 遮罩；**抬起不触发速记** | 合成按住时长 > 380ms（速记场景需留出书写时间）|
| `interceptScreenShotKey` → 380ms 定时器 | miui-services.jar | DOWN 后 380ms 仍未抬起 → 立即截屏，抬起被吞 | 合成按住 450ms 左右即可 |
| `StylusMask` 触摸监听（toolType==2 校验） | SecurityCoreAdd.apk | 速记的最终触发是**笔尖触摸遮罩**（toolType==TOOL_TYPE_STYLUS 的 ACTION_DOWN），与按键无关 | 用户必须持笔书写；桥接器无法代替此步 |
| `MiuiStylusDeviceListener.onInputDeviceAdded` | miui-services.jar | 虚拟设备注册即触发：`Settings.Secure setting_stylus_version` 写入、`ITouchFeature.setTouchMode(0,20,ver|16)`、防误触 blocker 使能 | 无需额外动作；注意 blocker 会随虚拟设备常驻而常开 |
| 设置页设备扫描（`C0278l` 反射 + `C.a()` 名称匹配） | SecurityCoreAdd.apk | 枚举所有 InputDevice，反射调用 `isXiaomiStylus()`；类型 UI 另按设备名匹配 | uinput 名称设为官方名称（见上表）|

---

### 设备侧（伪装对象：你的实体笔 → 桥接器的输入契约）

| penbridge 符号 | 期望的内核事件 | 说明 |
|---|---|---|
| `PEN_VENDOR / PEN_PRODUCT / PEN_NAME_TAG` | `EVIOCGID` / `EVIOCGNAME` | 实体笔按键接口的定位三要素,**移植到其他笔时只需改这三个宏**（如某笔的双击上报不同扫描码，另改 `PEN_SCAN_*`）|
| `PEN_SCAN_SINGLE`（联想 0x000c0600）| `MSC_SCAN` value + `KEY_*` 按下沿 | 单击手势的固件扫描码,若你的笔固件**不区分**单击/双击（只发同一扫描码），需要把本项目的"按扫描码分派"改为基于时间窗的双击判定状态机 |
| `PEN_SCAN_DOUBLE`（联想 0x000c0601）| 同上 | 双击手势的固件扫描码（联想固件把一次双击合并为单对 down/up 上报）|
| `KEY_UNKNOWN_LNX`（联想 240）| `EV_KEY` code | 手势事件挂载的键值,部分笔直接上报 `KEY_BUTTON_*` 或可映射键值，改此宏即可 |
| `synth_press` 分派表 | — | 单击/双击分别映射到 `KEY_PAGEDOWN`/`KEY_PAGEUP`,如果你的目标机型框架键位不同（如海外版速记键为其他键码），改这两处 |
| `create_virtual_pen` 的 ID/名称 | `UI_DEV_SETUP` | 换伪装对象时同步改 `VIRT_VENDOR/VIRT_PRODUCT/VIRT_NAME`，并确认上表功能面差异 |

---

### 已知边界（改造成其他设备时注意）

- **防误触 blocker 常开**：虚拟笔存活期间 native 掌托压制（`delayTime=40ms`）持续生效，桥接器无法关闭（`persist.stylus.palm.reject` 是全局开关）,触控面板兼容性需实机体感确认
- **`ITouchFeature.setTouchMode`**：虚拟设备注册会触发 `setTouchMode(0,20,ver|16)`；目标设备没有 v1/v2 HIDL touch HAL 时报 `NoSuchElementException` 并被忽略（良性），有则可能改变触摸行为
- **息屏语义**：`interceptQuickNoteKeyDownWhenScreenOff()` 恒返回 false——熄屏时按键成为唤醒键，速记不会触发（框架设计如此）
- **电池/配对/OTA**：属于小米私有 BLE（MIPP/OOB）能力，与输入通道无关，虚拟化不覆盖；如需电量显示可另行桥接 `com.android.settings.stylus.STYLUS_BATTERY_NOTIFY` 广播
