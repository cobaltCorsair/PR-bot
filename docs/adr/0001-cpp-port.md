# ADR-0001: C++ desktop port architecture

**Status:** Proposed

**Date:** 2026-10-21

**Deciders:** Project owner

## Context

The existing application is a PyQt5 desktop GUI that automates browser interactions through Selenium. Its Python entry point depends on legacy Selenium APIs, and its checked-in ChromeDriver is a Windows executable. The requested port should retain the desktop workflow and browser behavior while supporting a locally installed browser driver.

## Decision

Use C++17 with Qt 6 Widgets for the desktop UI and Qt Network for the W3C WebDriver HTTP protocol. Launch a user-provided ChromeDriver executable and keep forum logic in a worker thread. Store ordinary settings in JSON, but keep account passwords in memory only.

## Options Considered

### Qt 6 Widgets plus WebDriver protocol

| Dimension | Assessment |
|-----------|------------|
| Complexity | Medium to high |
| Cost | No runtime fee; Qt availability is required |
| Scalability | Suitable for a single-user desktop tool |
| Team familiarity | Closest C++ match to the existing PyQt interface |

**Pros:** Keeps a familiar desktop UI; uses the browser's existing JavaScript behavior; avoids relying on removed Selenium APIs.

**Cons:** The project must maintain a small WebDriver client and install Qt and a matching ChromeDriver separately.

### Qt WebEngine

| Dimension | Assessment |
|-----------|------------|
| Complexity | Medium |
| Cost | No runtime fee; larger Qt dependency |
| Scalability | Suitable for a single-user desktop tool |
| Team familiarity | Similar UI framework, different browser automation model |

**Pros:** Browser rendering and the UI can live in one Qt process.

**Cons:** Adds the large Qt WebEngine runtime and changes browser/session behavior from the existing Selenium approach.

### Keep Python and repair the current implementation

| Dimension | Assessment |
|-----------|------------|
| Complexity | Low to medium |
| Cost | No runtime fee |
| Scalability | Suitable for current use |
| Team familiarity | Existing implementation |

**Pros:** Lowest-risk way to restore current behavior.

**Cons:** Does not meet the requested C++ rewrite.

## Trade-off Analysis

Qt 6 preserves the existing desktop interaction model, while the W3C WebDriver protocol provides browser control from C++ without an official Selenium C++ client. Managing ChromeDriver as an external executable avoids bundling a platform-specific binary. The account password is intentionally excluded from settings files.

## Consequences

- The C++ app requires Qt 6, CMake, Chrome, and a compatible ChromeDriver.
- Forum-specific selectors and login scripts still need validation against the user's actual forums.
- The Python version remains available as a reference until the C++ behavior is validated.
- The C++ build cannot be confirmed in an environment without Qt and CMake.

## Action Items

1. [x] Implement the Qt UI, configuration handling, WebDriver adapter, and bot workflow.
2. [x] Add an offline smoke test for settings and password handling.
3. [ ] Build and run the application in an environment with Qt 6 and CMake.
4. [ ] Validate selectors and login behavior against a non-production forum before replacing the Python version.
