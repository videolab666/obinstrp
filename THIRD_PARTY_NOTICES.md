# Third-party notices

Pitel Instant Replay is distributed under the GNU General Public License; see `LICENSE`.

## Historical project provenance

This project was originally developed from the public `Voodoo25/obs-sports-replay` repository, which is licensed under GPL-2.0. During the Pitel Instant Replay redesign, the legacy RAM/file replay runtime was removed and the remaining inherited runtime components were either removed or independently reimplemented for the current disk/session/Event architecture. The historical origin is recorded here and in `docs/UPSTREAM_CLEANUP.md` so that provenance is not lost when the project is moved to a standalone repository.

The cleanup does not remove or supersede obligations imposed by the GPL on code or history to which those obligations apply.

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
