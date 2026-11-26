#pragma once

// A helper program's protocol streams (the browser helper, docs/browser.md). No SDL, so CEF's helper links it
// without the client.

#include <cstdio>

namespace hailstone::platform {

// The protocol's streams, taken off standard input and output: private copies of them, in binary mode (Windows
// would otherwise turn the protocol's 0x0a bytes into 0x0d 0x0a), closed across exec on POSIX and not inheritable on
// Windows, so no process this one starts gets them. Standard input and output (the C descriptors 0 and 1 and, on
// Windows, the process's standard handles) read and write the null device from then on: CEF's renderers, GPU and
// utility processes, which inherit those, can neither write into the client's stream nor read its messages, and a
// library's stray print to standard output cannot break the framing. False when it could not; the program should
// then end rather than speak over streams its children share.
struct ProtocolStreams {
    std::FILE* in = nullptr;
    std::FILE* out = nullptr;
};
bool take_protocol_stdio(ProtocolStreams& streams);

// Whether a process started now would get this stream's descriptor: no close-on-exec on POSIX, an inheritable handle
// on Windows. False for a null stream.
bool stream_inherited_by_children(std::FILE* f);

} // namespace hailstone::platform
