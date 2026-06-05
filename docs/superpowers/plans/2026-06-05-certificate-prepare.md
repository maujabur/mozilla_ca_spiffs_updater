# Certificate Prepare Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Python CLI that generates `bundle_ca.bin` for the updater from Mozilla `certdata.txt`, with local override support.

**Architecture:** `tools/certificate_prepare/prepare.py` handles input resolution, NSS-to-PEM conversion, delegation to ESP-IDF `gen_crt_bundle.py`, and output validation. Tests exercise pure conversion and orchestration with temporary files and fake subprocess calls.

**Tech Stack:** Python 3 standard library, `unittest`, ESP-IDF `gen_crt_bundle.py`.

---

### Task 1: Conversion And CLI Tests

**Files:**
- Create: `tests/tools/certificate_prepare/test_prepare.py`
- Create: `tools/certificate_prepare/prepare.py`

- [ ] **Step 1: Write failing tests**

Create tests for parsing a minimal trusted certdata object, rejecting missing trusted certs, using local override paths, and validating max size.

- [ ] **Step 2: Run tests to verify red**

Run: `python -m unittest tests.tools.certificate_prepare.test_prepare -v`

Expected: fail because `tools.certificate_prepare.prepare` does not exist.

- [ ] **Step 3: Implement minimal script**

Implement `prepare.py` with functions:

- `convert_certdata_to_pem(certdata_text: str) -> str`
- `resolve_gen_crt_bundle(explicit_path: str | None) -> Path`
- `run_gen_crt_bundle(gen_script, pem_path, work_dir, max_certs) -> Path`
- `validate_output(path, max_size) -> None`
- `main(argv=None) -> int`

- [ ] **Step 4: Run tests to verify green**

Run: `python -m unittest tests.tools.certificate_prepare.test_prepare -v`

Expected: all tests pass.

### Task 2: Documentation

**Files:**
- Create: `tools/certificate_prepare/README.md`
- Modify: `README.md`

- [ ] **Step 1: Add tool README**

Document default download mode, local `--certdata` override mode, required ESP-IDF environment, and output file.

- [ ] **Step 2: Link root README**

Add a short pointer from the root README to `tools/certificate_prepare`.

- [ ] **Step 3: Run verification**

Run: `python -m unittest tests.tools.certificate_prepare.test_prepare -v`

Expected: all tests pass.
