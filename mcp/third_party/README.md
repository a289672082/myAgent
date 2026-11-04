# Vendored dependencies

The directories below contain complete source checkouts from their upstream repositories, including headers, examples, tests, documentation and original license files. Git history (`.git`) is intentionally excluded.

| Local directory | Upstream source | Pinned commit |
| --- | --- | --- |
| `third_party/jsoncons` | https://github.com/danielaparker/jsoncons | `91c5e03f9169f2accecf3d5c082163d6a2f34b18` |
| `third_party/cpp-httplib` | https://github.com/yhirose/cpp-httplib | `a18895f42e4768062b858c6847b1ae598b2a3be3` |

The original upstream licenses are preserved at `third_party/jsoncons/LICENSE` and `third_party/cpp-httplib/LICENSE`. Review and preserve those notices when redistributing the SDK.

## Offline build

```sh
cmake -S . -B build -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake **never downloads** these libraries. It uses the checked-in directories and fails with a clear error if they are missing. OpenSSL, CMake, a C++20 compiler and Threads remain system/toolchain prerequisites.

Installed SDK distributions also include the required jsoncons and cpp-httplib headers plus upstream licenses.

## Updating these dependencies

Review the upstream change and license, update the pinned commit, regenerate the complete checked-out source directory (exclude only its `.git` metadata), then test on Linux and Windows before merging.
