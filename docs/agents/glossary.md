# Hailstone contributor glossary

| Term | Meaning in this repository |
| --- | --- |
| RoF2 | Rain of Fear 2, the original client and protocol baseline used for game rules. [Mechanics](../mechanics.md) |
| Stock EQEmu | An unmodified EQEmu server using the RoF2 client protocol. Use it as the base compatibility path when considering extensions. [Plan](../plan.md) |
| Live assets | Files from a current EverQuest installation. This is an asset source and is independent of the server or protocol under test. [RoF2 or live](../layout.md#rof2-or-live) |
| Live session | An actual run against a server. Name the type of server and the actions and results observed when reporting it. [Validation](validation.md) |
| Login, world and zone | Stages with distinct messages and state. Login establishes access, world handles character selection, and entering a zone hands off to gameplay. [Login protocol](../protocol/login.md) |
| Opcode, decoder and handler | The opcode identifies a message, the decoder reads its bytes, and downstream handling applies the result or presents it. Follow all relevant stages when tracing a symptom. [RoF2 packets](../protocol/rof2-packets.md) |
| WLD and EQG | Asset formats behind different content paths. A visual report should identify the asset path it exercises. [WLD](../formats/wld.md) and [EQG](../formats/eqg.md) |
| UI pack and Lua bridge | A UI pack defines presentation and interaction. The client bridge supplies shared data, drawing and actions through the documented UI API. Modern and Gamepad are the shipped packs. [Modular UI API](../ui-api.md) |
| Descriptor | A declaration of the shared UI surface, used by tooling and tests to check the public contract and generate its reference. [Adding a client feature](../ui-api.md#adding-a-new-client-feature) |
| TLO | Top-Level Object. A named entry point for a MacroQuest-style Lua script to query client state through the `mq` module. [TLOs](../plugins.md#tlos) |
| SDK and ABI | The SDK defines the public compiled plugin interface. The ABI is its binary contract, including layout, version and calling conventions. [Compiled plugins](../plugins.md#compiled-plugins) |
| Add-on and capability | An add-on is distributed separately and declares its requested capabilities in its manifest. The client and server rules govern what it can use in a session. [Capability ceiling](../addons.md#capabilities-the-manifests-caps-are-a-ceiling) |
| Asset folder and data folder | The asset folder is the player's read-only game installation. Hailstone stores its own settings, logs and per-profile data in its data folder. [Folder layout](../layout.md) |
| Recorded vectors and mock server | Vectors are external recorded input for selected tests. The mock server supplies controlled local protocol flows in integration tests. Report which was used. [CI and local builds](../ci.md#the-tests) and [mock server](../../tests/mock_server/README.md) |
| Worktree, base and head | A worktree is a checkout sharing Git objects with the main clone. The base is the revision or branch used for comparison, and the head is the revision being reviewed. [Shared workflow](workflow.md#start-or-resume-a-task) |
