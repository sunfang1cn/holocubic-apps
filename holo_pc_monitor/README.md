# Holo PC Monitor

Holo PC Monitor 是一款面向 HoloCubic 320×240 屏幕的电脑硬件监控应用。应用直接连接 AIDA64 RemoteSensor，在设备上显示 CPU、GPU、内存、温度、频率和风扇转速，并提供天气、时间、日期和历史趋势曲线。

## 功能特点

- CPU、GPU 和内存使用率实时显示
- CPU/GPU 温度、频率与风扇转速监控
- 经典双环布局与 320×240 四卡片仪表盘切换
- 新增 320×240 蜂窝节点性能布局，不覆盖原有两种界面
- CPU、GPU 和温度历史趋势曲线
- 天气、时间和日期信息
- 中文网页控制页面，可配置数据地址和显示布局
- 温度与风扇多传感器备用机制：主传感器缺失或返回 `0` 时自动使用备用值

## AIDA64 设置

1. 打开 AIDA64。
2. 进入“文件 → 设置 → 硬件监控 → LCD”。
3. 启用 RemoteSensor，并设置监听端口。
4. 进入“LCD 项目”，导入 `package/holo-aida.rslcd`。
5. 点击“应用”，然后通过浏览器访问 `http://电脑IP:端口/sse` 检查数据。

正常情况下，页面会持续输出包含 `CPU Usage`、`GPU Temperature`、`Memory Usage` 等字段的 `data:` 数据。

## 传感器备用顺序

- CPU 温度：`CPU 二极管 / CPU Diode` → `CPU Temperature` → `CPU Package`
- GPU 温度：`GPU Temperature` → `GPU Diode` → `GPU1 Temperature`
- 风扇：`CPU Fan` → `GPU Fan` → `Chassis Fan 1/2` → 其他风扇
- 温度、频率和风扇值为 `0` 或未获取时，会继续查找下一项
- 所有来源均不可用时，界面显示 `--`

CPU、GPU 和内存使用率允许正常显示 `0%`。

## 安装到设备

将 `package` 目录中的文件上传至：

```text
/sd/apps/holo_pc_monitor/
```

最少需要以下文件：

```text
app.info
main.lua
aida_client.lua
config.lua
web.lua
main.png
info.html
holo-aida.rslcd
```

上传后重新扫描应用列表，并启动 `holo_pc_monitor`。

## 网页配置

在 HoloCubic 启动器中打开 Holo PC Monitor 的网页控制页面，填写运行 AIDA64 的电脑 IP、端口和 SSE 路径。常见地址格式为：

```text
http://192.168.0.80:80/sse
```

控制页面可以在经典双环、四卡仪表盘与蜂窝节点布局之间切换，设置会保存在设备的 `config.lua` 中。

## 文件说明

- `package/holo-aida.rslcd`：供用户导入 AIDA64 的正式 RemoteSensor 配置
- `package/holo-aida-required-metrics.rslcd`：与正式配置内容同步的开发参考文件
- 两个 `.rslcd` 中的每个项目都设置了固定且唯一的 `<LBL>`，避免不同 AIDA64 版本使用不同默认名称
- `package/info.html`：应用商店介绍页，默认中文并支持英文切换
- `package/app.info`：应用名称、版本号、图标和简介

## 版本

当前版本：`1.0.4`

## 更新内容

### 1.0.4

- 新增 320×240 蜂窝节点性能布局，保留经典双环与四卡仪表盘，默认页面调整为四卡仪表盘。
- 网页控制页面新增 CPU/GPU 名称设置与蜂窝布局强调色选择，并限制强调色仅在蜂窝布局下配置。
- 扩展 AIDA64 指标：CPU 电压、CPU 封装功率、已用/可用内存、CPU/GPU 名称以及多网卡上传和下载速度。
- 优化 AIDA64 标签匹配与备用值逻辑，修复 CPU 温度误读功率、CPU 二极管温度、风扇及网速获取异常。
- 更新两份 `.rslcd` 配置并统一固定标签，移除未使用的显存占用项目，提升不同 AIDA64 版本的兼容性。
- 优化蜂窝布局的文字自适应、对齐、间距、分隔线和状态信息显示。

### 1.0.3

- 新增蓝牙手柄支持：按下 Select 或 Home 可退出应用并返回桌面。
- 增加 AIDA64 中文标签兼容，CPU 温度优先识别 `CPU 二极管`，同时兼容 `CPU Diode`、`CPU Temperature` 和 `CPU Package`。
- 完善风扇传感器名称兼容，支持 CPU、GPU、机箱风扇及备用 CPU 风扇标签。
- 修复网页控制页面保存配置后中文传感器别名丢失的问题。
- 保留 `.rslcd` 中固定且唯一的导出名称，降低不同 AIDA64 版本默认标签差异造成的影响。

许可证：GPL-3.0
