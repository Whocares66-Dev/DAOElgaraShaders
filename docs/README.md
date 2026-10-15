# The guide

The player's guide, a Jekyll site in `/docs` beside the source, as the other DAO repositories' are (Just the Docs v0.12.0; DAOPhotoCam's parts, 2026-10-15). It is published to GitHub Pages by `.github/workflows/docs.yml` on every push to master that touches it and on every release tag (`tools/build_docs.py`). Developer notes are in `/dev`.

## Preview

```sh
cd docs
bundle install
bundle exec jekyll serve
```

Then open `http://127.0.0.1:4000/DAOElgaraShaders/`. Every version, as the site is published: `python tools/build_docs.py --serve` from the repository's root.

## Editing

`index.md` and the pages beside it, written by hand; each page's `nav_order` is its place in the sidebar. Clear and short, for players: what each option does and how to choose, not how it works (the user, 2026-10-15: "be clear & succinct, don't have to explain technical detail"). One line a paragraph, as everywhere in this repository.

## Pictures

`assets/img/` holds the pictures, from the game's own frames at 1600 x 1000, in the `elgara-agent` instance on 2026-10-15, with master at 56d5fca:

| Picture | From |
|---|---|
| `cailan.jpg`, `blooddragon.jpg`, `runic.jpg` | one save and one photo cam pose each, a launch of vanilla (no plugin) and one with every option on; cropped about the figure, side by side |
| `video-tab.png` | the options screen's Video tab, its Elgara Shaders section cropped |
