# MCP direct third-party dependencies (no myMcpTools submodule)

The repository tracks upstream source **directly** as pinned Git submodules:

| Location (repository root) | Upstream | Pinned commit |
| --- | --- | --- |
| `third_party/jsoncons` | https://github.com/danielaparker/jsoncons | `91c5e03f9169f2accecf3d5c082163d6a2f34b18` |
| `third_party/cpp-httplib` | https://github.com/yhirose/cpp-httplib | `a18895f42e4768062b858c6847b1ae598b2a3be3` |

The original upstream licenses are retained at
`third_party/jsoncons/LICENSE` and `third_party/cpp-httplib/LICENSE`.
CMake reads the local source tree without network access; initialize submodules first:

```sh
git submodule sync --recursive
git submodule update --init --recursive
cmake -S mcp -B build-mcp -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build build-mcp
ctest --test-dir build-mcp --output-on-failure
```

The integrated MCP SDK source resides in `mcp/sdk/`; it no longer references
`third_party/myMcpTools` nor uses the retired `MYAGENT_MCP_DIR` variable.
