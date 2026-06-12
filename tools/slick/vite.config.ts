import { defineConfig } from "vite";
import preact from "@preact/preset-vite";

export default defineConfig({
  plugins: [preact()],
  // Deployed under slick.slopclaude.com root; relative base also survives
  // being served from a subpath if we ever move it.
  base: "./",
  build: {
    target: "es2022",
    sourcemap: true,
  },
});
