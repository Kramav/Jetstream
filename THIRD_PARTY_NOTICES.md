# Third-party notices

remod includes the following third-party software and data. Their full licence texts are in the `licenses`
folder of the release zip (sources: each vcpkg port's `copyright` file, and `third_party/` in the repository).

| Component | Used for | Licence | Source |
|---|---|---|---|
| Dear ImGui 1.92.9 (docking) | the app's interface | MIT | https://github.com/ocornut/imgui |
| imgui-node-editor 0.9.3 | the app's graph canvas | MIT | https://github.com/thedmd/imgui-node-editor |
| DirectXTex 2026-05-07 | texture decoding, mips and encoding | MIT | https://github.com/microsoft/DirectXTex |
| DirectXMath | used by DirectXTex | MIT | https://github.com/microsoft/DirectXMath |
| toml++ 3.4.0 | game profiles | MIT | https://github.com/marzer/tomlplusplus |
| nlohmann/json 3.12.0 | graph files, the API | MIT | https://github.com/nlohmann/json |
| nativefiledialog-extended 1.4.0 | the app's file pickers | zlib | https://github.com/btzy/nativefiledialog-extended |
| libopus 1.6.1 | writing the game's Opus audio (WEM) | BSD 3-clause | https://opus-codec.org |
| libvorbis 1.3.7 | writing the game's Vorbis audio (WEM) | BSD 3-clause | https://xiph.org/vorbis |
| libogg 1.3.6 | used by libvorbis | BSD 3-clause | https://xiph.org/ogg |
| Lua 5.4.8 | checking REFramework scripts' syntax | MIT | https://www.lua.org |
| ww2ogg's Wwise Vorbis codebooks (`packed_codebooks_aoTuV_603.bin`) | writing the game's Vorbis audio | BSD 3-clause (Xiph.org Foundation, Adam Gashlin) | https://github.com/hcs64/ww2ogg (commit 14ed9b0) |
| `vcomp140.dll` | DirectXTex's OpenMP runtime | Microsoft Visual C++ Redistributable terms | Visual Studio |

Catch2 is used by the tests only and is not shipped.
