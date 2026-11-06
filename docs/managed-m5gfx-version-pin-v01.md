# Managed M5GFX version constraint

The pinned M5Unified submodule declares `m5stack/m5gfx >=0.2.22`. The
project's main component now adds the exact constraint `0.2.36`, restricting
that managed dependency without editing either submodule or generated files.
This constraint applies to board configurations that use the main manifest;
target firmware validation here is limited to the C152 development workflow.

The C152 workflow runs a read-only version guard after target configuration,
before compilation, and again after the build before packaging. The guard
requires the direct project scalar constraint and the prepared managed
component's version metadata both to equal `0.2.36`. Missing, ambiguous,
unsupported or mismatched metadata fails the command and prevents packaging.
It neither downloads components nor changes manifests, lock files or sources.

The guard intentionally accepts only the current simple scalar manifest
format. It does not parse general YAML, replace the IDF component manager,
verify package content hashes, or lock the rest of the dependency graph.
Dependency solving and the full firmware build remain separate validations.

Offline fixture tests exercise read-only success, version drift, relaxed
constraints, missing or duplicate metadata, nested-key rejection, and command
exit behavior. The existing host workflow discovers these tests through its
`tools/controlled-close/test_*.py` command. No additional job or workflow is
introduced. Existing workflow timeouts and build-input preparation remain.

This change bounds a build input; it does not diagnose or repair ticket HTTP
connection failures and does not change runtime authorization or close policy.
