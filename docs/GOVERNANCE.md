# Governance

## Project model

esp32-zapret is a small, focused project maintained by a single maintainer
(a "benevolent dictator for now" model). The maintainer, **@Fairen8**, is the
final decision maker for the project and is responsible for releases, security
handling and repository administration.

## Roles and responsibilities

| Role | Who | Responsibilities |
|---|---|---|
| Maintainer | @Fairen8 | Final decisions, code review, releases, security response, repository/CI administration, badge criteria maintenance |
| Contributors | anyone | Submit bug reports, feature ideas and pull requests via the `community` branch |
| Users | anyone | Report issues, provide field feedback (provider/region, DPI behavior) |

## Decision process

1. Ideas and bugs are discussed in GitHub issues (or in PR comments).
2. Changes are proposed as pull requests. Changes to `main` must pass 6 CI
   checks (`unit tests`, `static analysis`, `repo hygiene`, two ESP-IDF build
   configurations and `fuzz smoke test`) and are merged by the maintainer.
3. Security-relevant changes are additionally reviewed against the threat model
   in [SECURITY_DESIGN.md](SECURITY_DESIGN.md).
4. User-visible changes are recorded in `CHANGELOG.md`; releases follow
   [Semantic Versioning](https://semver.org/lang/ru/).

Community pull requests are submitted to the `community` branch, require a
green CI run and an explicit approval from the maintainer.

## Access continuity

- The repository is hosted on GitHub and is public; all source code, history,
  CI configuration and release artifacts remain available without any action
  from the maintainer.
- Repository access is protected by a GitHub account with two-factor
  authentication; no other accounts have write access.
- All project infrastructure is reproducible from the public repository:
  CI workflows are in `.github/workflows/`, the release process is documented
  in [MAINTENANCE.md](MAINTENANCE.md), and the toolchain is standard ESP-IDF.
- The project is MIT-licensed; if the maintainer becomes unavailable, anyone
  may fork and continue development (see `LICENSE`).

## Code of conduct

All participation is covered by [CODE_OF_CONDUCT.md](../CODE_OF_CONDUCT.md).

## Communication

- Bugs and feature requests: GitHub issues.
- Security reports: private GitHub Security Advisories
  (`SECURITY.md`).
