# Hailstone's licence

Hailstone is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 2 of the License, or (at your option) any later
version. It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See [LICENSE](LICENSE) for the GNU General Public
License, version 2.

Copyright (C) 2026 gennro and the Hailstone contributors.

## What is not under the GPL

- **The plugin SDK** (the [sdk/](sdk/) folder: the `hailstone_*.h` headers, the older `nmsc_*.h` names and the
  UI pack example in `sdk/ui/`) is under the MIT licence in [sdk/LICENSE](sdk/LICENSE), so a plugin may include it
  whatever its own licence.
- **Third-party code** bundled with or fetched by the build keeps its own licence, listed in
  [THIRD_PARTY.md](THIRD_PARTY.md).

## The plugin and add-on exception

As a special exception, the copyright holders of Hailstone give you permission to make, use and distribute plugins,
scripts, UI packs and other add-ons for Hailstone under terms of your choice, and to have Hailstone load and run them,
without those add-ons being covered by the GNU General Public License, on condition that the add-on talks to
Hailstone only through:

- the plugin SDK in [sdk/](sdk/) (a compiled plugin built against `hailstone_plugin.h`, using the host functions,
  callbacks, top-level objects and packets it describes), or
- the documented Lua interfaces (the `mq` module, the `hs.*` API, also named `eq.*` as it was up to 0.7, and the UI
  pack files described in [docs/plugins.md](docs/plugins.md) and [docs/dev-ui.md](docs/dev-ui.md)).

An upscaler provider built against `hailstone_upscaler.h` is such an add-on, and the exception holds for it too
when it loads a separately licensed runtime of its own (a graphics vendor's upscaler library) and ships that
runtime beside it, under that runtime's terms.

An add-on that copies code from Hailstone outside [sdk/](sdk/), or is linked with it in any other way, is not
covered by this exception, and the GPL applies to it as usual. This exception does not change what Hailstone
itself is under. If you modify Hailstone, you may extend this exception to your version, but you are not obliged
to; if you do not want to, delete this section from your version.

## Before the GPL

Hailstone was released under the MIT licence up to and including 0.7.0-beta (the repository's history up to the
commit that brought in this file). Those releases and that code remain available under the MIT licence, which
permits including it in this GPL work. Its notice is kept here, as the MIT licence asks:

```
MIT License

Copyright (c) 2026 gennro

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
