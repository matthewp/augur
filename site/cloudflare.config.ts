import { defineConfig } from "cf/config";

// augur.gemwm.org: Augur's website, plain HTML from public/ (see
// wrangler.config.ts). `npm run dev` serves it locally; `npm run deploy`
// publishes it.
export default defineConfig({
  worker: {
    name: "augur-site",
    compatibilityDate: "2026-10-01",
    assets: {
      htmlHandling: "auto-trailing-slash",
      notFoundHandling: "404-page",
    },
    domains: ["augur.gemwm.org"],
  },
});
