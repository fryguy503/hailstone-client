# UI starter

Copy this folder to `ui/my-interface/` next to Hailstone and restart the client.
Choose **Example Interface** from the Interface list. Rename it in `ui.ini`.

`base=modern` supplies the remaining windows and helper libraries. This example
replaces `player.lua` and reads the shared `hs.UI` API. Add another file with
the same relative name to replace an inherited window or helper. Keep game
state and actions in the host API; keep presentation in the pack.

In the Hailstone source checkout, `docs/ui-api.md` describes discovery,
compatibility and the client-feature author workflow. The installed client does
not need to include those source documents. The original example code is MIT licensed
under the SDK's license; it contains no game artwork or source layouts.
