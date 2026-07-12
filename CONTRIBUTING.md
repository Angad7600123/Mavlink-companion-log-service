# Contributing

Thank you for your interest in **MAVLink Companion Service**. Contributions
are welcome, and anyone may submit a pull request. Before you do, please read
this document in full, especially the [Contribution Terms](#contribution-terms),
which govern the rights you grant when you submit a contribution.

**Repository:** [github.com/Angad7600123/Mavlink-companion-log-service](https://github.com/Angad7600123/Mavlink-companion-log-service)

> Copyright (c) 2026 Angad Singh Bains. All rights reserved.
> This project is distributed under the **Angad Singh Personal & Non-Commercial
> Source Available License**. See [LICENSE](LICENSE).

## Table of Contents

- [Contribution Terms](#contribution-terms)
- [How to Contribute](#how-to-contribute)
- [Development Setup](#development-setup)
- [Coding Guidelines](#coding-guidelines)
- [Documentation Guidelines](#documentation-guidelines)
- [Commit Message Style](#commit-message-style)
- [Branch Naming](#branch-naming)
- [Testing Expectations](#testing-expectations)
- [Pull Request Process](#pull-request-process)

## Contribution Terms

These terms are a condition of contributing. **By submitting a pull request or
any other contribution to this project, you agree to all of the following.**
If you do not agree, do not submit a contribution.

### Ownership and copyright

- Submitting a contribution does **not** transfer ownership of the project. The
  project owner retains all ownership rights in the project.
- You retain copyright in your own contribution.

### License grant to the project owner

By submitting a contribution, you automatically grant the project owner (Angad
Singh Bains) a **perpetual, worldwide, irrevocable, royalty-free, and
sublicensable** right to:

- use,
- modify,
- redistribute,
- relicense,
- commercially license,
- merge,
- remove, and
- rewrite

your contribution, in whole or in part, in this project and in any derivative or
future version of it, under any license terms the project owner chooses.

### Rights reserved by the project owner

The project owner may, at their sole discretion:

- relicense future versions of the project,
- make future versions closed source,
- sell commercial licenses,
- dual-license the project,
- accept, reject, or defer any contribution,
- rewrite or modify any contribution, and
- remove any contribution.

### Your assurances

By submitting a contribution, you confirm that:

- the contribution is your original work, or you have the necessary rights to
  submit it under these terms, and
- the contribution does not knowingly infringe the rights of any third party.

These contribution terms are consistent with, and supplementary to, the project
[LICENSE](LICENSE).

## How to Contribute

1. Search existing issues and pull requests to avoid duplication.
2. For anything beyond a small fix, open an issue first to discuss the approach.
3. Fork the repository and create a topic branch from `main`.
4. Make focused changes that address a single concern.
5. Add or update tests and documentation as appropriate.
6. Ensure the test suite passes.
7. Open a pull request that references the relevant issue.

## Development Setup

```bash
sudo apt install build-essential cmake libsqlite3-dev

git clone https://github.com/Angad7600123/Mavlink-companion-log-service.git
cd Mavlink-companion-log-service

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DMCLS_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Network access is required on the first configure so that build dependencies can
be fetched.

## Coding Guidelines

- The project targets **C++20**.
- Follow the existing style; a `.clang-format` configuration is provided. Run it
  before submitting:

  ```bash
  clang-format -i path/to/changed/file.cpp
  ```

- Separate responsibilities cleanly and avoid global mutable state.
- Prefer RAII and smart pointers; avoid manual memory management.
- Reserve exceptions for unrecoverable initialization failures; handle runtime
  errors gracefully.
- Use `std::filesystem` for filesystem operations.
- Avoid busy-waiting; use blocking I/O or condition variables.
- Keep public classes documented with a brief description of their purpose.
- Do not introduce a new third-party dependency without prior discussion.

## Documentation Guidelines

- Update documentation in the same pull request as the code it describes.
- Map of what to update:

  | Change | Update |
  |--------|--------|
  | Configuration keys | `config/config.toml`, [docs/configuration.md](docs/configuration.md), [README.md](README.md) |
  | Protocol behavior | [docs/protocol.md](docs/protocol.md), [docs/durability.md](docs/durability.md) |
  | Architecture | [docs/architecture.md](docs/architecture.md) |
  | User-facing workflow | [README.md](README.md) |
  | Any release | [CHANGELOG.md](CHANGELOG.md) |

- Use clear, professional prose and correct Markdown. Cross-reference related
  files where helpful.

## Commit Message Style

- Write concise, imperative subject lines (for example, "Add probe-hash
  deduplication").
- Keep the subject line under roughly 72 characters.
- Use the body to explain the reasoning behind a change when it is not obvious.
- Reference issues where applicable (for example, "Fixes #42").

## Branch Naming

Use short, descriptive, hyphenated branch names with a category prefix:

- `feature/` for new functionality, e.g. `feature/post-archive-hook`
- `fix/` for bug fixes, e.g. `fix/partial-cleanup-on-start`
- `docs/` for documentation-only changes, e.g. `docs/configuration-reference`
- `refactor/` for internal restructuring, e.g. `refactor/storage-pipeline`
- `test/` for test-only changes, e.g. `test/received-ranges`

## Testing Expectations

- All existing tests must pass before a pull request is reviewed.
- New behavior should include unit tests where practical. The suite lives under
  `tests/` and covers configuration parsing, the catalog database, received-range
  gap logic, and hashing.
- Hardware-in-the-loop testing against a real flight controller is encouraged
  for protocol-level changes but is not required for every contribution.

```bash
cmake -S . -B build -DMCLS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Pull Request Process

1. Rebase your branch onto the latest `main`.
2. Confirm the build and the full test suite pass locally.
3. Complete the checklist below.
4. A maintainer will review; changes may be requested, rewritten, or declined at
   the project owner's discretion in line with the [Contribution Terms](#contribution-terms).

### Checklist

- [ ] Tests pass (`ctest --test-dir build --output-on-failure`)
- [ ] Code follows the project conventions and is formatted
- [ ] Documentation updated for user-visible changes
- [ ] [CHANGELOG.md](CHANGELOG.md) updated under the unreleased section
- [ ] No secrets, credentials, or machine-specific paths included
- [ ] You agree to the [Contribution Terms](#contribution-terms)
