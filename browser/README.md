# The browser interface

This is the part of Pacific you see: the Silica interface with its tabs, toolbar, menus, settings, bookmarks,
history and password vault. It is Atlantic's interface, which grew out of Jolla's
[sailfish-browser](https://github.com/sailfishos/sailfish-browser), with the WebKit layer replaced by one that
talks to the Ladybird engine.

Where things are:

| Path | What is there |
|---|---|
| `apps/ladybird/` | the link to the engine: the page view, the tabs' pages, frames on the GPU, downloads, location |
| `apps/browser/qml/` | the pages and components of the interface |
| `apps/shared/` | QML shared between pages: the web view, the window, the overlay's animations |
| `apps/core/` | the browser object, settings, favicons |
| `apps/history/`, `apps/storage/` | tabs and history, kept in SQLite on a worker thread |
| `apps/browser/settings/` | the password vault and the login models |
| `apps/browser/bookmarks/` | bookmarks |
| `apps/lib/` | the interface's start-up |
| `apps/launcher/` | the app's one executable, which starts either the interface or an engine process |
| `data/searchEngines/` | the search engines it ships |

It is built with `scripts/build-browser.sh` against the Sailfish OS SDK's sysroot; see
[docs/BUILDING.md](../docs/BUILDING.md). How it fits with the engine is described in
[docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md).

When code comes over from sailfish-browser, keep its copyright header, note which upstream commit it came from,
and credit the author in the commit (`Co-authored-by:`).

MPL-2.0, as upstream; see [LICENSE.txt](LICENSE.txt).
