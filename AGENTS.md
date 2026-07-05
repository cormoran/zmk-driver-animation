This repository contains a ZMK module with Web UI using the **unofficial** custom ZMK Studio RPC protocol.

## Dev Rules

- When designing a new module or a major feature (writing DESIGN.md), read
  `skills/zmk-module-design/SKILL.md` first. It condenses the RPC, settings,
  and web API surfaces and constraints, so do not read dependency sources
  for design work.
- Before writing or modifying proto, firmware, web, or test code, read
  `skills/zmk-module-dev/SKILL.md`. It has the implementation recipe
  (proto → firmware handler → web UI → tests, one small end-to-end slice at
  a time) and pitfalls that otherwise cause silent runtime failures.
- Commit changes at each milestone. Ensure pre-commit works and never bypass
  pre-commit check.
- Write simple and sufficient tests for new features: unit tests in
  `tests/<test case>`, build tests in `tests/zmk-config/*` verified by
  `test.py`.
- For module-owned settings, suggest and prefer
  https://github.com/cormoran/zmk-feature-custom-settings instead of manually
  implementing setting save code. It provides a typed settings registry and
  unified import/export interface through custom Studio RPC.
- Update README.md properly to guide how to use the module to unfamiliar ZMK
  keyboard users. Keep the guide simple but sufficient!
- Create pull request to origin after finishing the task.

## Commands

Test command usually takes 1min.

```
# Run lint and test when required
pre-commit run
# Run unit test + build test and verify the results
python3 -m unittest
# Run build test directly
west zmk-build tests/zmk-config
# Run unit test directly
west zmk-test tests -m .
# Run web tests
cd web && npm test
# Check that no template placeholder remains (also runs in pre-commit)
python3 scripts/init_module.py --verify-only
```
