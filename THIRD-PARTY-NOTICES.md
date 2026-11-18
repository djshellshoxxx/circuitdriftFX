# Third-party notices

CircuitDrift plugin builds use the components below. Versions are pinned in `cmake/Dependencies.cmake`; the build
decision record is `docs/adr/ADR-0001-build-formats-and-ids.md`. Full licence texts ship in the source trees fetched at
the pinned commits; release archives must include them (see `.github/workflows/release.yml`).

| Component | Pin | Licence | Used for | Distributed |
| --- | --- | --- | --- | --- |
| JUCE 8.0.17-security | `dabd5ad13cd869cba41046947fffc8e5ce5cd662` | Used under AGPLv3 (owner decision, ADR-0001 section 2; JUCE is also offered under the JUCE 8 EULA) | Plugin framework, VST3 and Standalone wrappers | Compiled into binaries |
| clap-juce-extensions | `3837f4a6ed2ab0ba46aba69ae5b7041c7c64ce74` | MIT | CLAP wrapper for JUCE plugins | Compiled into the .clap binaries |
| CLAP (`clap-libs/clap`) | `195b42a004144fab0b3cf95e9c067187d15365b7` | MIT | CLAP API headers | Compiled into the .clap binaries |
| clap-helpers (`clap-libs/clap-helpers`) | `a380c978f429103e08afc1d17e9efbf3e3c96ee2` | MIT | CLAP wrapper helper library | Compiled into the .clap binaries |
| VST3 SDK 3.8.0 (bundled inside JUCE) | as bundled in the JUCE pin | MIT (Steinberg Media Technologies, 2025; `modules/juce_audio_processors_headless/format_types/VST3_SDK/LICENSE.txt`) | VST3 interfaces | Compiled into the .vst3 binaries |
| pluginval (Tracktion) | version pinned in `.github/workflows/release.yml` | GPLv3 | Plugin validation in CI only | No, never packaged |

Notes:

* VST3 is a trademark of Steinberg Media Technologies GmbH. CLAP is an open plugin standard from the free-audio project.
* The release archive must contain the licence files of every component whose code is compiled into the binaries
  (JUCE AGPLv3/licence file, MIT notices for clap-juce-extensions, CLAP and clap-helpers, the VST3 SDK licence file).
* Adding or updating a dependency requires updating this file in the same change.
