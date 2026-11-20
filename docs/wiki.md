# The wiki

Hailstone's [GitHub wiki](https://github.com/projecthailstone/hailstone-client/wiki) is built from this folder. `docs/`
stays the source, so a change to the code and the page that describes it are still one commit (AGENTS.md "Docs travel
with the code"). The wiki is output and is not edited by hand. Each page starts with a comment naming its source file,
and the footer says where to edit.

## What `tools/sync_wiki.py` does

- Writes one page per document in its `PAGES` table, plus `Home` (the index, by section), `_Sidebar` and `_Footer`,
  and copies `docs/images/`.
- Rewrites links for the wiki:
  - a link to a document goes to its page;
  - a link to any other file in the repository goes to that file on GitHub;
  - a `docs/<name>.md` mention in text or in backticks becomes a link to its page;
  - a quoted section name right after it (`docs/layout.md "RoF2 or live"`) becomes the link's anchor when that
    heading exists;
  - code blocks are left as they are.
- Deletes pages it wrote before that it no longer writes (a document renamed or removed). Files it did not write are
  left alone.

A new document needs a line in `PAGES` (its section and page name); `--check` fails until it has one.

## Publishing

The wiki is a git repository of its own beside this one. Its first page has to be made once in the browser (Wiki,
Create the first page) before it can be cloned.

```sh
git clone https://github.com/projecthailstone/hailstone-client.wiki.git ../hailstone-client.wiki
tools/sync_wiki.py --out ../hailstone-client.wiki
cd ../hailstone-client.wiki && git add -A && git commit -m "Sync from hailstone-client <commit>" && git push
```

Sync after a merge to `main` that changes `docs/`, `CONTRIBUTING.md` or `server/README.md`.

`tools/sync_wiki.py --check` writes nothing. It exits 1 when:

- a document is not in `PAGES`;
- a `PAGES` entry names a file that is gone;
- a markdown link points at a file that does not exist.

A plain `docs/` mention of a file that is not here is left as text. It is often another repository's (the NMS
plugin's own `docs/nms.md`, which is not in this repository).
