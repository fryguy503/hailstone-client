#!/usr/bin/env python3
"""Generate UI API names, capability requirements and verified binding references."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
ROW = re.compile(r'^\s*UI_(METHOD|CONSTANT)\("([\w.]+)",\s*"(\w+)",\s*"(\w+)"(?:,\s*(.*))?\)\s*$', re.M)
REGISTRAR = re.compile(r'\bvoid\s+(?:\w+::)?(register_functions|register_\w+_api|register_ui_extensions)\s*\([^;{}]*\)\s*\{')
DEFINITION = re.compile(r'\bint\s+(\w+)\s*\(\s*lua_State\s*\*\s*\w*\s*\)\s*\{')
PAIR = re.compile(r'\{\s*"(\w+)"\s*,\s*&?\s*(\w+)\s*\}')
REG_TABLE = re.compile(r'\bluaL_Reg\s+\w+\s*\[\s*\]\s*=\s*\{'
                       r'|\bstd::initializer_list\s*<\s*std::pair\s*<\s*const\s+char\s*\*\s*,\s*lua_CFunction\s*>\s*>\s*\{')
RAW_STRING = re.compile(r'R"([^ ()\\\t\r\n]{0,16})\(')

# Hand-written notes a module's generated table cannot carry.
NOTES = {
    "map": "An add-on also draws overlays with `hs.UI.map.RegisterOverlay` (docs/ui-api.md \"Map overlays\"): they run at\n"
           "the end of each drawn `hs.DrawZoneMap` and `hs.DrawMinimap`, whichever add-on's `map.lua` or `minimap.lua`\n"
           "called it, by priority and registration order.\n",
}


def masked_cpp(text):
    """Keep source offsets/braces, ignoring comments and string contents for bounded source scans."""
    result, i = list(text), 0
    while i < len(text):
        start = i
        if text.startswith("//", i):
            i = text.find("\n", i)
            if i < 0:
                i = len(text)
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = len(text) if end < 0 else end + 2
        elif text.startswith('R"', i) and (raw := RAW_STRING.match(text, i)) is not None:
            end = text.find(")" + raw[1] + '"', raw.end())
            i = len(text) if end < 0 else end + len(raw[1]) + 2
        elif text[i] in "\"'":
            quote = text[i]
            i += 1
            while i < len(text) and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            i = min(i + 1, len(text))
        else:
            i += 1
            continue
        for offset in range(start, i):
            if result[offset] != "\n":
                result[offset] = " "
    return "".join(result)


def body_end(masked, opening):
    depth = 1
    for offset in range(opening + 1, len(masked)):
        if masked[offset] == "{":
            depth += 1
        elif masked[offset] == "}":
            depth -= 1
            if depth == 0:
                return offset
    raise ValueError("Unclosed C++ registration function")


def adjacent_comment(text, offset):
    # Start at the definition's line, not its first token (which can have static/indentation before it).
    lines = text[:text.rfind("\n", 0, offset) + 1].splitlines()
    comment = []
    while lines and lines[-1].lstrip().startswith("//"):
        comment.append(lines.pop().lstrip()[2:].lstrip())
    return list(reversed(comment))


def implementation_index(root):
    """Map only actual public registrations to definitions; never infer ownership from a filename."""
    definitions, registrations, constants = defaultdict(list), defaultdict(list), {}
    for path in sorted((root / "src/app").glob("lua_ui*.cpp")):
        if path.name == "lua_ui_api.cpp":
            continue  # The catalog facade is not a flat host-binding registrar.
        text = path.read_text(encoding="utf-8")
        masked = masked_cpp(text)
        for match in DEFINITION.finditer(text):
            if masked[match.start():match.start() + 3] != "int":
                continue
            definitions[match[1]].append({"path": path, "line": text.count("\n", 0, match.start()) + 1,
                                          "comment": adjacent_comment(text, match.start())})
        for registrar in REGISTRAR.finditer(text):
            if masked[registrar.start():registrar.start() + 4] != "void":
                continue
            end = body_end(masked, registrar.end() - 1)
            body = text[registrar.end():end]
            mask = masked[registrar.end():end]
            prefix = ""
            if registrar[1] == "register_gamepad_api":
                exports = re.finditer(r'lua_setfield\s*\(\s*L\s*,\s*-2\s*,\s*"gamepad"\s*\)', body)
                if not any(mask[match.start()] == "l" for match in exports):
                    continue
                prefix = "gamepad."
            for table in REG_TABLE.finditer(body):
                if mask[table.start()] not in "ls":
                    continue
                table_end = body_end(mask, table.end() - 1)
                for pair in PAIR.finditer(body, table.end(), table_end):
                    if mask[pair.start()] == "{":
                        registrations[prefix + pair[1]].append((path, pair[2]))
            direct = re.compile(r'lua_pushcclosure\s*\(\s*L\s*,\s*(\w+)\s*,\s*\d+\s*\);\s*'
                                r'lua_setfield\s*\(\s*L\s*,\s*-\d+\s*,\s*"(\w+)"\s*\)')
            for match in direct.finditer(body):
                if mask[match.start():match.start() + 16] == "lua_pushcclosure":
                    registrations[prefix + match[2]].append((path, match[1]))
            integer = re.compile(r'lua_pushinteger\s*\(\s*L\s*,\s*(-?\d+)\s*\);\s*'
                                 r'lua_setfield\s*\(\s*L\s*,\s*-\d+\s*,\s*"(\w+)"\s*\)')
            for match in integer.finditer(body):
                if mask[match.start():match.start() + 15] == "lua_pushinteger":
                    constants[prefix + match[2]] = {"path": path,
                        "line": text.count("\n", 0, registrar.end() + match.start()) + 1, "value": match[1]}
    result = {}
    for binding, candidates in registrations.items():
        resolved = {}
        for owner, function in candidates:
            local = [item for item in definitions[function] if item["path"] == owner]
            matches = local or definitions[function]
            if len(matches) == 1:
                item = dict(matches[0], function=function)
                resolved[(item["path"], item["line"])] = item
        if len(resolved) == 1:
            result[binding] = next(iter(resolved.values()))
    return result, constants


def documented_excerpt(binding):
    if not binding:
        return []
    # Copy binding prose too: a comment can document a result without spelling out a call signature.
    # It stays explicitly labelled as a comment, never interpreted as an argument/result declaration.
    return binding.get("comment", [])


def code(value):
    fence = "`" * (1 + max((len(run) for run in re.findall(r"`+", value)), default=0))
    padding = " " if value.startswith("`") or value.endswith("`") else ""
    return fence + padding + value.replace("|", "\\|") + padding + fence


def binding_link(root, binding):
    relative = binding["path"].relative_to(root).as_posix()
    return f"[source](../{relative})" # the file, not a line: an edit elsewhere in it must not make the page stale


def render(root=ROOT, report=None):
    source = (root / "src/app/lua_ui_api.inc").read_text(encoding="utf-8")
    modules = defaultdict(list)
    binding_names, targets = set(), set()
    rows = ROW.findall(source)
    hooks = re.findall(r'^\s*UI_HOOK_POLICY\("([\w.]+)"\)\s*$', source, re.M)
    declared = sum(bool(re.match(r"\s*UI_(?:METHOD|CONSTANT)\s*\(", line)) for line in source.splitlines())
    if len(rows) != declared:
        raise ValueError("UI API catalog contains a row the reference generator cannot parse")
    for kind, binding, module, member, caps in rows:
        if binding in binding_names or (module, member) in targets:
            raise ValueError("duplicate UI API descriptor")
        binding_names.add(binding)
        targets.add((module, member))
        labels = []
        for token in (caps or "0").split("|"):
            token = token.strip().removeprefix("plugin::")
            if token == "0":
                continue
            if not re.fullmatch(r"kCap[A-Z]\w*", token):
                raise ValueError("unrecognized capability expression: " + caps)
            labels.append(token[4:].lower())
        modules[module].append((member, binding, kind, ", ".join(labels) or "none (outer gate)"))
    if not modules:
        raise ValueError("UI API catalog is empty")
    method_bindings = {binding for kind, binding, _, _, _ in rows if kind == "METHOD"}
    if len(hooks) != len(set(hooks)) or not set(hooks) <= method_bindings:
        raise ValueError("UI hook policy must name a unique registered method")
    bindings, constants = implementation_index(root)
    methods = [(module, member, binding) for module, entries in modules.items()
               for member, binding, kind, _ in entries if kind == "METHOD"]
    unresolved = sorted(f"API.{module}.{member}" for module, member, binding in methods if binding not in bindings)
    documented = sum(bool(documented_excerpt(bindings.get(binding))) for _, _, binding in methods)
    unresolved_constants = sorted(f"API.{module}.{member}" for module, entries in modules.items()
        for member, binding, kind, _ in entries if kind == "CONSTANT" and binding not in constants)
    coverage = {"methods": len(methods), "constants": len(rows) - len(methods),
                "verified_method_bindings": len(methods) - len(unresolved), "documented_excerpts": documented,
                "verified_constant_bindings": len(rows) - len(methods) - len(unresolved_constants),
                "unresolved_methods": unresolved, "unresolved_constants": unresolved_constants,
                "undocumented_methods": sorted(f"API.{module}.{member}" for module, member, binding in methods
                    if not documented_excerpt(bindings.get(binding)))}
    if report is not None:
        report.update(coverage)
    lines = ["<!-- Generated by tools/gen_ui_api_docs.py from the catalog and binding sources. Regenerate after edits. -->\n",
             "# UI API Reference\n\n",
             "Use `local API = hs.UI`, then call `API.<module>.<Member>(...)`.\n",
             "Arguments and results retain the corresponding flat `hs.*` method's documented meaning;\n",
             "see the [UI API guide](ui-api.md), [developer UI](dev-ui.md) and [script API](plugins.md).\n",
             "The catalog is the source of names and outer capability requirements. A zero outer mask\n",
             "does not bypass a function's own caller, screen or server-state checks.\n\n",
             "`API.version`, `API.Supports(module[, member])` and `API.Describe([module[, member]])`\n",
             "provide contract version and structural discovery. They confer no permissions.\n\n",
             f"Coverage: **{len(methods) - len(unresolved)}/{len(methods)} method implementation links**, ",
             f"**{documented}/{len(methods)} adjacent binding comment excerpts**, ",
             f"and **{coverage['verified_constant_bindings']}/{coverage['constants']} constant implementation links**. ",
             "Excerpts below are verbatim binding comments; ",
             "they preserve any documented signatures and results, and do not invent omitted arguments or results. ",
             "**See binding** marks an author documentation gap.\n\n",
             "Unresolved method implementations: " + (", ".join(code(name) for name in unresolved) or "none") + ".\n",
             "Unresolved constant implementations: " + (", ".join(code(name) for name in unresolved_constants) or "none") + ".\n\n"]
    for module in sorted(modules):
        lines += [f"## {module}\n\n", "| Member | Documented signature / binding comment | Existing binding | Required capabilities | Hook policy |\n",
                  "| --- | --- | --- | --- | --- |\n"]
        for member, binding_name, kind, caps in sorted(modules[module]):
            call = f"API.{module}.{member}"
            binding = bindings.get(binding_name) if kind == "METHOD" else constants.get(binding_name)
            excerpt = documented_excerpt(binding) if kind == "METHOD" else []
            signature = "<br>".join(code(line) for line in excerpt) if excerpt else "**See binding**"
            if kind == "CONSTANT" and binding:
                signature = code("= " + binding["value"])
            location = code("hs." + binding_name) + (" · " + binding_link(root, binding) if binding else " · **not located**")
            policy = "plain-capabilities" if binding_name in hooks else "closed"
            lines.append(f"| {code(call)} | {signature} | {location} | {caps} | {policy} |\n")
        lines.append("\n")
        if module in NOTES:
            lines += [NOTES[module], "\n"]
    return "".join(lines).rstrip() + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--report", action="store_true", help="Print signature/link coverage and unresolved methods as JSON")
    args = parser.parse_args()
    target = ROOT / "docs/ui-api-reference.md"
    report = {}
    result = render(report=report)
    if args.check:
        if not target.exists() or target.read_text(encoding="utf-8") != result:
            parser.exit(1, "UI API reference is stale; run tools/gen_ui_api_docs.py\n")
    else:
        target.write_text(result, encoding="utf-8", newline="\n")
    if args.report:
        print(json.dumps(report, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
