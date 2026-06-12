# Fonts (tooling assets)

`LXGWWenKaiLite-Regular.ttf` — LXGW WenKai Lite (OFL, see
LXGW-OFL-LICENSE.txt). Source for the CJK subsets embedded in launcher
UIs (sts2-linux-launcher, pocs/hk-launcher). Re-subset when UI strings
change:

```
pyftsubset tools/fonts/LXGWWenKaiLite-Regular.ttf \
    --text="$(cat your_ui_strings.txt)" \
    --layout-features='*' --no-hinting \
    --output-file=<target>/launcher_font_zh.ttf
```

Keep OFL-LICENSE.txt alongside any distribution that embeds a subset.
