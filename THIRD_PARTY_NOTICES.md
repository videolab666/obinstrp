# Third-party notices

Pitel Instant Replay is distributed under the GNU General Public License; see `LICENSE`.

## Historical project provenance

This project was originally developed from the public `Voodoo25/obs-sports-replay` repository, which is licensed under GPL-2.0. During the Pitel Instant Replay redesign, the legacy capture-time RAM/file replay runtime was removed and the architecture was changed to continuous disk recording, persistent Sessions, Events and replay buses.

The current repository contains substantial Pitel-developed Session/Event code as well as GPL-derived files or components whose original copyright notices remain in place. `docs/UPSTREAM_CLEANUP.md` records the removed/replaced runtime paths and the cleanup boundary. Moving the project to a new standalone GitHub repository or starting a new Git commit history does not erase provenance or change licensing obligations for code derived from earlier GPL material.

## OBS Studio and OBS plugin build infrastructure

Pitel Instant Replay is an OBS Studio plugin and uses APIs and build conventions from the OBS Project, including libobs, obs-frontend-api and OBS plugin-template-derived build infrastructure. OBS Studio and related OBS Project components are distributed under their respective licenses.

## FFmpeg

The media pipeline links to FFmpeg libraries including libavcodec, libavutil, libavformat and libswscale. FFmpeg components are distributed under their own LGPL/GPL licensing terms depending on the exact build and enabled components.

## Qt

The operator interface uses Qt 6. Qt is distributed under multiple licensing options; the applicable license depends on the Qt distribution used to build or package the plugin.

## SQLite

Event/session metadata uses the official SQLite amalgamation. SQLite is dedicated to the public domain by its authors; see the SQLite project for its current copyright and usage notice.

## Hardware and platform APIs

Hardware-accelerated paths may use vendor/platform interfaces exposed through FFmpeg, OBS Studio and the operating system, including NVIDIA NVENC, AMD AMF, Intel Quick Sync / D3D11VA and Microsoft Direct3D 11. Their names and trademarks belong to their respective owners.

This notice is informational and is not a substitute for the license texts distributed with the relevant third-party components.
