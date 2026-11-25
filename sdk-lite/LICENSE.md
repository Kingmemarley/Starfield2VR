# sdk-lite: license

The headers in this folder (the game's structures in `include/RE`, the `include/REL` relocation helpers and `include/stl.h`)
came into this project from Mutar's starfield2vr (https://github.com/mutars/starfield2vr). They very likely derive from
CommonLibSF (https://github.com/Starfield-Reverse-Engineering/CommonLibSF), whose code is under:

GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception (with Corresponding Source)

So these headers are distributed under those same terms: the GNU General Public License version 3 or later (`COPYING`, in this
folder) with CommonLibSF's exceptions (`EXCEPTIONS`, in this folder). The rest of the repository is under the MIT License (see
`LICENSE` at the repository root and `THIRD_PARTY_NOTICES.md`).

The compiled mod (`dxgi.dll`) includes code from these headers. Its complete source code, including these headers, is published
in this repository.
