# Maintenance

How to maintain and update the project: releases, dependencies, security
updates, CI, and badge criteria.

## Release process

1. Bump `VERSION` (semver) and `version` in
   `components/esp_desync/idf_component.yml`; add a `CHANGELOG.md` section.
2. Open a PR from `main` to `releases`; wait for the 6 required checks.
3. Copilot is auto-requested for review (ruleset `copilot-review-releases`) and
   its approval satisfies the required review. Merge once checks are green and
   Copilot has approved (maintainer bypass remains available).
4. The release workflow builds firmware, creates the `vX.Y.Z` tag and publishes
   the GitHub release with assets and SLSA provenance — but only after the
   maintainer **approves the `release` environment** (Actions → Review
   deployments). The process is idempotent: if the tag already exists,
   publishing is skipped.

## Dependency updates

- Dependabot proposes GitHub Actions updates (SHA-pinned) and security fixes.
- Dependabot alerts and automated security fixes are enabled.
- ESP-IDF version is pinned in CI (`.github/workflows/tests.yml`); bump it
  deliberately and re-run the full test suite.

## Security updates

- Private reports arrive via GitHub Security Advisories (`SECURITY.md`).
- Fix on `main` → PR to `releases` → approve → release; reference the advisory
  in `CHANGELOG.md`.

## CI overview

| Workflow | Trigger | Purpose |
|---|---|---|
| `tests` | push to `main`, PRs to `main`/`community`; called by `release` | unit tests, static analysis, repo hygiene, ESP-IDF builds (2 configs), fuzz smoke test |
| `release` | PR/push to `releases` | runs tests; publishes release after environment approval |
| `codeql` | all branch pushes, PRs, weekly | CodeQL analysis |
| `scorecard` | push to `main`, weekly, manual | OpenSSF Scorecard |
| `pr review request` | PRs to `community`/`releases` | requests maintainer review for external PRs |

## Release PR review (Copilot)

- Only PRs targeting `releases` get an automatic Copilot review request
  (ruleset `copilot-review-releases`; `review_on_push` and draft reviews are
  disabled to save AI credits — re-request manually when needed).
- One review per PR: if new commits are pushed, the `releases` protection
  dismisses the approval; click "Re-request review" on the PR.
- Copilot approvals count toward the required review: repository settings,
  Settings → Copilot → Code review (effort level **Lite**, auto-approval on).
- Review guidance: `.github/copilot-instructions.md` (Copilot reads it from the
  PR head branch).

## Badge criteria

The OpenSSF Best Practices answers are stored in `.bestpractices.json`
(repository root). The badge service reads this file when the maintainer opens
the edit pages ("Save (and continue) 🤖"). When project facts change (new
tests, coverage, governance), update the file and refresh the badge entry.

## Continuity

See [GOVERNANCE.md](GOVERNANCE.md#access-continuity): the repository, CI
configuration and release process are fully reproducible from the public
sources.
