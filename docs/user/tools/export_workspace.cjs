// Export the manual illustrations from the demo's localized sample history.
// Run with Playwright on NODE_PATH and Edge installed: node docs/user/tools/export_workspace.cjs
const fs = require("node:fs/promises");
const path = require("node:path");
const {pathToFileURL} = require("node:url");
const {chromium} = require("playwright");

function demoUrl(language, scene) {
  const demo = pathToFileURL(path.resolve(__dirname, "../../website/index.html"));
  demo.searchParams.set("demo", scene);
  demo.searchParams.set("lang", language);
  return demo.href;
}

async function save(name, language, dataUrl) {
  const suffix = language === "en" ? ".en" : "";
  await fs.writeFile(path.resolve(__dirname, `../assets/${name}${suffix}.png`), Buffer.from(dataUrl.split(",")[1], "base64"));
}

async function exportIllustrations() {
  const browser = await chromium.launch({channel: "msedge", headless: true});
  try {
    const page = await browser.newPage({viewport: {width: 752, height: 800}, deviceScaleFactor: 2});
    for (const language of ["zh-CN", "en"]) {
      await page.goto(demoUrl(language, "manual"));
      await page.locator('#workspace-stage [data-card="card-3"][data-part="body"]').waitFor();
      await page.evaluate(() => document.fonts.ready);
      const sheet = page.locator("#workspace-canvas");
      await save("workspace", language, await sheet.evaluate(canvas => canvas.toDataURL()));
      const hanging = await sheet.evaluate(canvas => {
        const bounds = canvas.getBoundingClientRect();
        const card = document.querySelector('#workspace-stage [data-card="card-1"][data-part="body"]').getBoundingClientRect();
        const scale = canvas.width / bounds.width;
        // Include the hook above the hit area and the shadow below the paper.
        const left = Math.max(0, card.left - bounds.left - 10);
        const top = Math.max(0, card.top - bounds.top - 32);
        const crop = document.createElement("canvas");
        crop.width = Math.round((card.right - bounds.left + 10 - left) * scale);
        crop.height = Math.round((card.bottom - bounds.top + 9 - top) * scale);
        crop.getContext("2d").drawImage(canvas, -Math.round(left * scale), -Math.round(top * scale));
        return crop.toDataURL();
      });
      await save("hanging-text", language, hanging);

      await page.goto(demoUrl(language, "floating"));
      const floating = page.locator('.floating-card-window:has([data-card="card-1"]) canvas');
      await floating.waitFor();
      await page.evaluate(() => document.fonts.ready);
      await save("floating-text", language, await floating.evaluate(canvas => canvas.toDataURL()));
    }
  } finally {
    await browser.close();
  }
}

exportIllustrations().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
