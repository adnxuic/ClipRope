# Verified development tasks

| Task | Verification |
| --- | --- |
| Hanging double-click in UI Automation editors | [Regression, release gate and visual review](paste-uia.md) |
| Resolve the UI comparison base after a release tag moves | Windows tag run [#4](https://github.com/adnxuic/ClipRope/actions/runs/43451087891) received the previous annotated tag object. A clean checkout must fetch that exact object, peel it to its original commit, and run the UI evidence check before the mandatory regressions. |
