// Export the demo's actual canvas, including its sample cards and native-port geometry.
// Run with Playwright on NODE_PATH and Edge installed: node docs/user/tools/export_workspace.cjs
const fs = require("node:fs/promises");
const path = require("node:path");
const {pathToFileURL} = require("node:url");
const {chromium} = require("playwright");

async function exportWorkspace() {
  const demo = pathToFileURL(path.resolve(__dirname, "../../website/index.html"));
  demo.searchParams.set("demo", "manual"); // Uses the demo's disposable in-memory history.
  const browser = await chromium.launch({channel: "msedge", headless: true});
  try {
    const page = await browser.newPage({viewport: {width: 640, height: 800}, deviceScaleFactor: 2});
    await page.goto(demo.href);
    await page.locator('#workspace-stage [data-card="card-3"][data-part="body"]').waitFor();
    await page.evaluate(() => document.fonts.ready);
    const png = await page.locator("#workspace-canvas").evaluate(canvas => canvas.toDataURL("image/png"));
    await fs.writeFile(path.resolve(__dirname, "../assets/workspace.png"), Buffer.from(png.split(",")[1], "base64"));
  } finally {
    await browser.close();
  }
}

exportWorkspace().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
