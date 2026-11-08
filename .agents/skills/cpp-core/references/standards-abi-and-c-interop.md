# C++ standards, toolchain, ABI and C interop

## Version contract

C++17 is the portable reference boundary for this skill; it was chosen because OBS Studio 33.0.1 sets CMAKE_CXX_STANDARD=17 and CMAKE_C_STANDARD=17 in cmake/common/compiler_common.cmake, with no extensions in CODESTYLE.md. This does not make OBS a dependency and does not establish a particular Qt 6 minor or target platform.

When reviewing a target, use its existing language configuration, effective per-target compile flags, SDK and CI, not online examples that happen to compile under a newer compiler. A C++20 range, concepts, designated initializer or std::jthread cannot be silently introduced into a C++17 contract. Compiler extensions accepted under -std=gnu++17 do not prove strict -std=c++17 compatibility. C17 applies to C translation units; C APIs have distinct language and ABI semantics.

## API versus ABI

- API includes public types, function signatures, error semantics and ownership requirements.
- ABI includes name mangling, calling conventions, alignment, packing, RTTI and exceptions policy, virtual layout, exported symbol sets, compiler/CRT/standard library ABI and allocator boundaries. A source-compatible change may still break binary consumers.
- For a native shared-library/plugin boundary, inspect actual host SDK and interface headers; verify lifetime of host-owned handles, callback registration/unregistration, API version negotiated at load and unload, allocator ownership and thread contract.
- Do not throw exceptions through a C callback or across a foreign/unsupported ABI. Convert to project-defined error/status at the boundary and preserve cleanup.
- extern "C" controls linkage; it does not automatically make C++ classes ABI-stable or enforce shared allocator compatibility.
- Hidden/default visibility, dllexport/dllimport, RPATH and install names are project/platform-specific. Do not assume one OS linker behavior on another.

## Concrete review cases

- A public class gains a new virtual member or changes data layout: examine ABI consumers, not only tests.
- A callback stores a pointer to a temporary std::string: determine whether the C library copies the bytes or retains them.
- A C allocator returns a buffer: pair deallocation with the owning API, not an arbitrary delete/free.
- A dynamic library built by a different compiler/CRT: prefer a verified C ABI where required; do not fabricate cross-toolchain ABI guarantees.
- C++ standard library objects crossing DLL/plugin boundaries: check compatibility of toolchain, runtime and ownership before approving.

For a changed public header, validate each supported target/configuration. Incomplete matrix is not full compatibility verification.
