# SecureAudioPlayer（最小打包说明）

本目录是播放器与打包脚本的最小工程单元。  
目标是：生成 `packs/config/lines` + 嵌入资源，并编译 `SecureAudioPlayer.exe`。

## 1. 最小工程文件（建议纳入 Git）

- `secure_player.cpp`：主程序
- `SecurePlayer.pro`：Qt 工程文件
- `build_secure_player.bat`：编译脚本
- `pack_secure_audio.ps1`：加密打包主脚本
- `pack_secure_audio.bat`：打包入口（可选）
- `make_install_package.ps1`：安装包流程脚本
- `SecureAudioPlayer_resource.rc`、`app_icon.ico`：图标与资源

## 2. 打包输入目录（音频素材来源）

`pack_secure_audio.ps1` 默认以 `aa` 的上级目录为素材根目录，读取：

- `../00concat`（中文站名语音）
- `../00concatEng`（英文站名语音）
- `../template`（模板音频）
- `../00lines`（线路文件，xlsx/txt）
- `../00config`（配置 json）

兼容规则：

- 若 `./00config` 存在且有 json，会优先从 `./00config` 读取配置。

## 3. 打包输出目录（运行必需）

执行 `pack_secure_audio.ps1` 后会更新：

- `./packs`：加密后的 `*.pak`
- `./config`：模板配置 + `pack_manifest.json`
- `./lines`：线路文件
- `./seed_config.zip`、`./seed_lines.zip`：默认配置种子
- `./embedded_payload.qrc`：Qt 资源索引

其中运行时真正依赖的是 `packs/config/lines`（以及程序首次解压使用的 seed zip）。

## 4. 常用命令

```powershell
# 1) 打包加密资源
powershell -ExecutionPolicy Bypass -File .\pack_secure_audio.ps1 -ShowProgress

# 2) 编译 exe
.\build_secure_player.bat
```

编译产物：

- `./release/SecureAudioPlayer.exe`

## 5. 目录清理建议（最小单位）

以下属于中间产物，不建议纳入版本管理：

- `debug/`、`dist/`、`_installer_work/`、`player_src/`
- `Makefile*`、`.qmake.stash`
- `release/` 下全部编译产物
- `packs/*.pak`（体积大，建议按需本地生成）
