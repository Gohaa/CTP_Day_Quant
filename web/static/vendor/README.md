# Vendored front-end libs (no CDN)

## lightweight-charts.standalone.production.js

- Upstream: https://github.com/tradingview/lightweight-charts
- Version: 5.0.8 (Apache-2.0)
- File: npm package `dist/lightweight-charts.standalone.production.js` (IIFE ¡ú `window.LightweightCharts`)
- Refresh:

```bash
python -c "import urllib.request,tarfile,pathlib; p=pathlib.Path('.'); t=p/'_t.tgz'; urllib.request.urlretrieve('https://registry.npmjs.org/lightweight-charts/-/lightweight-charts-5.0.8.tgz', t); tar=tarfile.open(t); m=next(x for x in tar if x.name.endswith('lightweight-charts.standalone.production.js')); open('lightweight-charts.standalone.production.js','wb').write(tar.extractfile(m).read()); t.unlink()"
```

Usage in HTML (served by `web/server.py`):

```html
<script src="/static/vendor/lightweight-charts.standalone.production.js"></script>
```
