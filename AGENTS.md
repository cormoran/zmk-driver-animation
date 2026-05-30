This repository contains a ZMK module.

## Dev Rules

- Commit changes at each milestone. Ensure pre-commit works and never bypass pre-commit check.
- Write simple and sufficient tests for new features.
  - Unit test: Test major functionalities as much as possible by adding case to `tests/<test case>`.
    You might have to add test only logic like executing logic at zephyr initialization to improve test coverage.
  - Build test: Enable feature in `tests/zmk-config/*`. It is to verify build works for real device and to easily test with real device.
    Ensure the feature and device is enabled as expected in the build by verifying output in `test.py`.
- Update README.md properly to guide how to use the module to unfamiliar ZMK keyboard users. Keep the guide simple but sufficient!
- Create pull request to origin after finishing the task

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
```
