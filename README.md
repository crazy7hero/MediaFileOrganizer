# 媒体文件整理工具 Media File Organizer

面向个人创作者和小团队的**媒体文件扫描、筛选、复制一体化工具**。

## 能解决什么问题

- 📷 摄影师：SD 卡素材快速归档到多个备份硬盘
- 🎬 Up主/自媒体：按分辨率/时长/编码筛选视频，整理 2TB 素材库
- 👨‍👩‍👧 家庭用户：多台电脑的照片视频合并整理
- 🏢 小型工作室：项目结束后的素材归档和交付

## 功能

- 多源目录扫描（支持多硬盘/U盘同时扫描）
- 文件列表展示（排序、勾选、右键菜单、元数据列）
- 媒体元数据提取（分辨率、时长、编码格式 — 基于 MediaInfo）
- 按类型和元数据筛选（全部/视频/图片/音频 + 高级筛选）
- 多目标并行复制（每个目标独立进度，支持随时取消）
- 磁盘类型自动检测（HDD 串行 / SSD 并行 / NVMe 并行）
- 保留原始时间戳 / 跳过已存在文件 / 磁盘空间不足警报
- 配置自动保存（路径、筛选条件、窗口布局）
- 方案管理（一键切换工作场景）
- 筛选预设（保存/加载常用筛选条件）
- 导出文件清单（TXT/CSV）
- 纯本地运行，无需联网

## 编译环境

| 依赖 | 版本 |
|------|------|
| Qt | 5.15.2 (MinGW 64-bit) |
| 编译器 | MinGW 8.1.0 (x86_64) |
| qmake | Qt 5.15.2 自带 |
| MediaInfo | DLL 动态库 (项目根目录) |
| 操作系统 | Windows 7/10/11 (64-bit) |

### 编译步骤

```bash
# 1. 安装 Qt 5.15.2 (MinGW 64-bit)
# 2. 将 MediaInfo.dll 放到项目根目录
# 3. 编译
qmake untitled.pro -spec win32-g++ "CONFIG+=release"
mingw32-make -j4

# 4. 收集依赖
windeployqt release/untitled.exe
cp MediaInfo.dll release/
```

## 目录结构

```
├── src/
│   ├── core/       # 核心逻辑（线程池、磁盘检测、扫描器、复制引擎、元数据）
│   ├── model/      # 数据模型（文件列表、筛选代理）
│   ├── ui/         # 界面（主窗口）
│   └── thirdparty/ # 第三方头文件（MediaInfoDLL.h）
├── doc/            # 用户手册
├── MediaInfo.dll   # 运行时依赖
├── untitled.pro    # Qt 项目文件
└── README.md
```

## 用户手册

详见 [doc/用户手册.md](doc/用户手册.md)

## 下载地址
https://github.com/crazy7hero/MediaFileOrganizer/releases/download/v1.5.0/MediaFileOrganizer_v1.5.zip

点链接直接下载，解压后双击 MediaFileOrganizer.exe 即可运行

## License

MIT License

Copyright (c) 2026

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
