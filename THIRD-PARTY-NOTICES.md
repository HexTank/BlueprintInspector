# Third-party notices

BlueprintInspector is MIT-licensed (see [`LICENSE`](LICENSE)). It links two third-party libraries and
vendors their public headers under `Source/ThirdParty/include`.

| Component | Version | License | Text |
|---|---|---|---|
| [google/cppdap](https://github.com/google/cppdap) | commit `6464cd7` | Apache License 2.0 | [`Source/ThirdParty/LICENSE-cppdap.txt`](Source/ThirdParty/LICENSE-cppdap.txt) |
| [microsoft/Detours](https://github.com/microsoft/Detours) | commit `adb0760` | MIT | [`Source/ThirdParty/LICENSE-detours.md`](Source/ThirdParty/LICENSE-detours.md) |

The prebuilt libraries under `Source/ThirdParty/lib/{v142,v143}` are stripped builds of those projects.
Rebuild them from the upstream commits above if you need to step into their sources. The headers under
`Source/ThirdParty/include` keep their original copyright notices.

The v143 `cppdap.lib` must be built with the same MSVC line the engine uses. UE 5.5 to 5.8 ship with
14.4x. Use CMake with the Ninja generator, `Release`, the default `/MD` runtime, no debug information,
and the pinned `6464cd7` commit. Rebuild the pair when the engine's MSVC line moves.

The VS Code extension does not bundle either library.
