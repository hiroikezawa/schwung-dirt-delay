import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
/* The page engine comes from a Schwung checkout:
 * SCHWUNG_ROOT, else ../schwung beside this repository. */
const here = path.dirname(fileURLToPath(import.meta.url));
const root = process.env.SCHWUNG_ROOT || path.resolve(here, "../../schwung");
const pp = (f) => import(pathToFileURL(path.join(root, "src/shared/param_pages", f)).href);
const { validateContract } = await pp("validate_contract.mjs");
const { planPages, PAGE_KNOBS, pageSlotKeys } = await pp("page_plan.mjs");
const contract = JSON.parse(fs.readFileSync(new URL("../build/native/runtime-contract.json", import.meta.url)));
const manifest = JSON.parse(fs.readFileSync(new URL("../module.json", import.meta.url)));
assert.deepEqual(contract.chainParams, manifest.capabilities.chain_params);
const result = validateContract({id: "dirt-delay", ...contract, capabilities: manifest.capabilities});
assert.deepEqual(result.findings.filter(f => f.level === "error" || f.level === "warn"), []);
const plan = planPages(contract);
const pages = (Array.isArray(plan) ? plan : plan.pages).filter(p => p.kind === PAGE_KNOBS);
/* One page, eight knobs: nothing to page to. */
assert.equal(pages.length, 1);
assert.deepEqual(pageSlotKeys(pages[0]), ["mix", "drive", "time", "feedback", "crush", "gain", "fx1", "fx2"]);
console.log("PASS: Schwung planner: one knob page, Mix on knob 1, Input Gain on knob 6, no contract warnings");
