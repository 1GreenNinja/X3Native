import { render } from "preact";
import { App } from "./app";
import { initTheme } from "./theme";

initTheme(); // apply any saved Appearance theme before first paint
render(<App />, document.getElementById("app")!);
