#!/usr/bin/env python3
"""Migrate shipped Lua packs to hs.UI using the host's explicit API descriptor.

Only Lua code tokens are changed; comments, short/long strings and layout stay
as authored. Unknown direct hs calls fail closed for a deliberate review.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
ROW = re.compile(r'^\s*UI_(METHOD|CONSTANT)\(\s*"([\w.]+)"\s*,\s*"(\w+)"\s*,\s*"(\w+)"', re.M)


@dataclass(frozen=True)
class Token:
    text: str
    start: int
    end: int


def catalog(path: Path) -> dict[str, tuple[str, str]]:
    rows = ROW.findall(path.read_text(encoding="utf-8"))
    if not rows:
        raise ValueError("API descriptor contains no UI_METHOD/UI_CONSTANT rows")
    result, targets = {}, set()
    for _, binding, module, member in rows:
        if binding in result or (module, member) in targets:
            raise ValueError(f"Duplicate API descriptor: {binding} -> {module}.{member}")
        result[binding] = (module, member)
        targets.add((module, member))
    return result


def tokens(text: str) -> list[Token]:
    result, i, size = [], 0, len(text)

    def long_end(start: int) -> int | None:
        opening = re.match(r"\[(=*)\[", text[start:])
        if not opening:
            return None
        close = "]" + opening[1] + "]"
        end = text.find(close, start + opening.end())
        if end < 0:
            raise ValueError("Unterminated Lua long string/comment")
        return end + len(close)

    while i < size:
        if text[i].isspace() or text[i] == "\ufeff":
            i += 1
        elif text.startswith("--", i):
            end = long_end(i + 2)
            if end is None:
                end = text.find("\n", i + 2)
                if end < 0:
                    end = size
            i = end
        elif text[i] in "\"'":
            quote, start = text[i], i
            i += 1
            while i < size and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
            if i >= size:
                raise ValueError("Unterminated Lua quoted string")
            i += 1
            result.append(Token(text[start:i], start, i))
        elif text[i] == "[" and (end := long_end(i)) is not None:
            result.append(Token(text[i:end], i, end))
            i = end
        elif text[i].isalpha() or text[i] == "_":
            start = i
            i += 1
            while i < size and (text[i].isalnum() or text[i] == "_"):
                i += 1
            result.append(Token(text[start:i], start, i))
        else:
            start = i
            if text.startswith("...", i):
                i += 3
            elif text.startswith("..", i):
                i += 2
            else:
                i += 1
            result.append(Token(text[start:i], start, i))
    return result


def migrate(text: str, mapping: dict[str, tuple[str, str]]) -> str:
    code = tokens(text)
    replacements = []
    unknown = []
    for index, token in enumerate(code):
        if token.text == "eq" and not (index > 0 and code[index - 1].text in (".", ":")):
            line = text.count("\n", 0, token.start) + 1
            raise ValueError(f"Deprecated UI namespace at line {line}; use hs.UI")
        if token.text != "hs" or index > 0 and code[index - 1].text in (".", ":"):
            continue
        if index + 2 >= len(code) or code[index + 1].text != ".":
            continue
        first = code[index + 2]
        if first.text == "UI":
            continue
        binding, end = first.text, first.end
        unmapped_nested = False
        if index + 4 < len(code) and code[index + 3].text == ".":
            nested = binding + "." + code[index + 4].text
            if nested in mapping:
                binding, end = nested, code[index + 4].end
            elif first.text == "gamepad":
                binding, unmapped_nested = nested, True
        if binding in mapping:
            module, member = mapping[binding]
            replacements.append((token.start, end, "API." + module + "." + member))
        elif first.text == "gamepad" and not unmapped_nested and any(name.startswith("gamepad.") for name in mapping):
            replacements.append((token.start, first.end, "API.gamepad"))
        else:
            line = text.count("\n", 0, token.start) + 1
            unknown.append(f"hs.{binding} at line {line}")
    if unknown:
        raise ValueError("Unmapped binding API references: " + ", ".join(unknown))
    if not replacements:
        return text
    has_alias = False
    for index, token in enumerate(code[:-1]):
        if token.text == "local" and code[index + 1].text == "API":
            declaration = [t.text for t in code[index + 2:index + 7]]
            if declaration[:4] != ["=", "hs", ".", "UI"]:
                raise ValueError("Local API name is already used for another value")
            has_alias = True
    if not has_alias:
        at = code[0].start if code else len(text)
        newline = "\r\n" if "\r\n" in text else "\n"
        replacements.append((at, at, "local API = hs.UI" + newline))
    for start, end, value in sorted(replacements, key=lambda item: (item[0], item[1]), reverse=True):
        text = text[:start] + value + text[end:]
    return text


def mock_adapter(path: Path) -> str:
    """Test fixtures resolve their explicit flat mocks dynamically; discovery never uses their __index."""
    source = path.read_text(encoding="utf-8")
    policy = (ROOT / "src/plugin/policy.h").read_text(encoding="utf-8")
    bits = {name: 1 << int(shift) for name, shift in re.findall(r"\b(kCap\w+)\s*=\s*1u\s*<<\s*(\d+)", policy)}
    rows = []
    hooks = set(re.findall(r'^\s*UI_HOOK_POLICY\("([\w.]+)"\)\s*$', source, re.M))
    for line in source.splitlines():
        match = ROW.match(line)
        if not match:
            continue
        kind, binding, module, member = match.groups()
        caps = 0
        if kind == "METHOD":
            expression = line[match.end():].strip().lstrip(",").rsplit(")", 1)[0].strip()
            for term in expression.split("|"):
                term = term.strip().removeprefix("plugin::")
                if term != "0":
                    if term not in bits:
                        raise ValueError(f"Unknown capability expression in descriptor: {term}")
                    caps |= bits[term]
        values = (binding, module, member, "function" if kind == "METHOD" else "constant")
        rows.append("  {" + ", ".join(json.dumps(value) for value in values) + f", {caps}, " +
                    ("true" if binding in hooks else "false") + "},")
    return '''-- Generated by tools/migrate_ui_api.py --mock-adapter from src/app/lua_ui_api.inc.
-- Test-only adapter: production aliases are the host's finalized C++ closures.
local descriptors = {
''' + "\n".join(rows) + '''
}
return function(root)
  assert(type(root) == "table", "A mock hs table is required")
  local function lookup(path, raw)
    local value = root
    for part in path:gmatch("[^.]+") do
      if type(value) ~= "table" then return nil end
      if raw then value = rawget(value, part) else value = value[part] end
    end
    return value
  end
  local API, modules = {version={major=1,minor=7}}, {}
  for _, row in ipairs(descriptors) do
    local module = modules[row[2]]
    if not module then module={};modules[row[2]]=module end
    module[row[3]] = row
  end
  for name, members in pairs(modules) do
    API[name] = setmetatable({}, {__index=function(_, member)
      local row = members[member]
      return row and lookup(row[1], false) or nil
    end})
  end
  function API.Supports(module, member)
    local members = type(module)=="string" and modules[module]
    if not members then return false end
    if member==nil then return true end
    local row = members[member]
    if not row then return false end
    local value = lookup(row[1], true)
    return row[4]=="function" and type(value)=="function" or row[4]=="constant" and value~=nil
  end
  local function describeMember(row)
    return {name=row[3],binding=row[1],kind=row[4],requiredCaps=row[5],hookable=row[6],
      hookPolicy=row[6] and 'plain-capabilities' or 'closed',
      hookReason=row[6] and '' or 'This method has not been approved for hook argument/result exposure'}
  end
  local function describeModule(name)
    local members=modules[name]
    if not members then return nil end
    local names, result={}, {name=name,methods={}}
    for member in pairs(members) do names[#names+1]=member end
    table.sort(names)
    for _, member in ipairs(names) do result.methods[#result.methods+1]=describeMember(members[member]) end
    return result
  end
  function API.Describe(module, member)
    if module~=nil then
      if not modules[module] then return nil end
      if member~=nil then local row=modules[module][member];return row and describeMember(row) or nil end
      return describeModule(module)
    end
    local names,result={}, {version={major=1,minor=7},modules={}}
    for name in pairs(modules) do names[#names+1]=name end
    table.sort(names)
    for _, name in ipairs(names) do result.modules[#result.modules+1]=describeModule(name) end
    return result
  end
  root.UI=API
  return API
end
'''


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pack", choices=("modern", "gamepad"), action="append", help="Repeat to select packs; default both")
    parser.add_argument("--catalog", type=Path, default=ROOT / "src/app/lua_ui_api.inc")
    parser.add_argument("--check", action="store_true", help="Check that migration has no pending changes")
    parser.add_argument("--mock-adapter", action="store_true", help="Generate only the shared Lua mock facade fixture")
    args = parser.parse_args(argv)
    try:
        mapping = catalog(args.catalog)
        if args.mock_adapter:
            path = ROOT / "tests/lua_ui_api_mock.lua"
            after = mock_adapter(args.catalog)
            before = path.read_text(encoding="utf-8") if path.exists() else ""
            if before != after:
                if not args.check:
                    path.write_text(after, encoding="utf-8", newline="\n")
                print(path.relative_to(ROOT).as_posix())
            return int(args.check and before != after)
        changed = []
        for pack in dict.fromkeys(args.pack or ("modern", "gamepad")):
            for path in sorted((ROOT / "ui" / pack).rglob("*.lua")):
                with path.open("r", encoding="utf-8", newline="") as stream:
                    before = stream.read()
                try:
                    after = migrate(before, mapping)
                except ValueError as error:
                    raise ValueError(f"{path.relative_to(ROOT)}: {error}") from error
                if before != after:
                    changed.append(path)
                    if not args.check:
                        with path.open("w", encoding="utf-8", newline="") as stream:
                            stream.write(after)
        for path in changed:
            print(path.relative_to(ROOT).as_posix())
        print(f"{len(changed)} {'pending' if args.check else 'migrated'} Lua files")
        return int(args.check and bool(changed))
    except (OSError, ValueError) as error:
        parser.exit(2, f"Migration failed: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
