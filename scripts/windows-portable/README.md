# chiaki-dlss Windows x64 portable

Extract the whole ZIP into a writable folder and run **chiaki-dlss.exe**. Pair your own PS5 in the app. The preconfigured LAN request is 1080p HEVC at 50 Mbps. The application stores settings and your own registrations under its local `data` directory.

## RTX Video and frame generation

Run **Install-NVIDIA-Runtime.cmd** once on a supported NVIDIA PC, then open **chiaki-dlss.exe**. Setup retrieves the original [Veyra 2.1.9 NVIDIA release](https://github.com/Likely7/Veyra-NRVideo/releases/tag/v2.1.9), verifies its SHA-256, and installs the selected runtime files into this package. The download is 697 MiB. If you already have that archive, you can run the script with `-ArchivePath` and its full path. Use a current NVIDIA driver and enable VRR on your display when available.

| Video preset | PS5 input | Client processing |
| --- | --- | --- |
| `RtxVideo4K60` | SDR, 30 fps | RTX Video quality 4 to 3840×2160 and VFG 2× |
| `RtxVideo4K` | SDR, 30 fps | RTX Video quality 4 to 3840×2160 |
| `Native` | SDR, 60 fps | Direct presentation |
| `Hdr60` | HDR10, 30 fps | Native HDR10, PQ and gamut calibration, VFG 2× |

The app starts in `RtxVideo4K60`. It can connect and stream at the native rate before runtime setup. Change presets from Windows Terminal. The selected preset persists on subsequent launches.

```powershell
.\chiaki-dlss.exe --video-preset Hdr60
```

Press **Ctrl+H** in HDR10 mode to tune PQ tone mapping, peak, black level and display gamut. The package includes the tested 1400-nit setting. Use **Reset** for your display's reported peak and black level. For a direct borderless connection, add `--borderless-fullscreen stream` followed by the nickname and address of your own registered console.

## Source

The source revision and linked library source references are in `BUILD-INFO.txt`. License texts accompany the program. The video provider uses [Veyra source at v2.1.9](https://github.com/Likely7/Veyra-NRVideo/tree/v2.1.9).
