# The docs site

The site at `gh-pages/public/` is **built from the Markdown this repo already
carries** — `README.md`, `HANDOFF.md`, `PLAN.md` and everything under `docs/`.
Nothing is duplicated here. Editing a document is the only step.

```console
$ pip install 'markdown-it-py==3.0.0'
$ python3 gh-pages/build_site.py
$ python3 gh-pages/check_links.py
```

Then open `gh-pages/public/index.html`, or serve the directory.

## How it fits together

| file | role |
|---|---|
| `site.json` | the site's identity, the home-page groups, and the card copy for each document |
| `templates/` | `home.html`, `doc.html`, `page.html` — plain `{{ name }}` substitution, no template engine |
| `assets/` | `site.css` and the logo, copied into the build |
| `build_site.py` | renders the Markdown, builds the nav and the cards, rewrites links |
| `check_links.py` | fails the build if the site links to something it does not contain |
| `public/` | the built site — **committed**, and checked against a fresh build in CI |

## Two things worth knowing before editing

**A document does not have to be registered.** Anything dropped into `docs/` is
discovered and published. Registering it in `site.json` only chooses its card
copy and which home-page group it sits in — so the site cannot silently omit a
document somebody wrote.

**`public/` is committed, and CI diffs it against a fresh build.** That is
deliberate: the built site can be read straight from the repo without a build
step, but a committed copy can go stale, and a stale site is invisible until
somebody reads it. So the workflow rebuilds from source and *fails* if the
committed output differs. What gets published is always the fresh build. After
editing any document:

```console
$ python3 gh-pages/build_site.py && git add gh-pages/public
```

`markdown-it-py` is pinned for the same reason — the staleness check diffs
generated HTML, so an unpinned renderer would fail the build on its own release
schedule.

## Publishing

Set **Settings → Pages → Source** to *GitHub Actions* once. After that
`.github/workflows/pages.yml` builds and deploys on every push to `main` that
touches a document, the templates or the assets.

## Provenance

The machinery — the builder, the link checker, the templates — was lifted from
[`s5fs`](https://github.com/nyxcraft/s5fs) and retargeted. The palette was not:
`s5fs` is green phosphor and this is the amber of a PDP-10 console, so the two
sibling sites are told apart at a glance.
