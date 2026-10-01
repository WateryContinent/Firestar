# Project document regression tests

These tests compile the actual editor document implementation using the vendored
RapidJSON headers. They need a C++20 compiler, but not the Windows GUI or Oodle.
They cover failed loads preserving the current project (including unsaved edits
and build-list state), saving after a rejected load, and successful replacements.

From the repository root on Linux:

```sh
g++ -std=c++20 -Wall -Wextra -Wpedantic -Isrc -isystem src/repak/thirdparty \
    tests/ProjectDocumentTests.cpp src/editor/ProjectDocument.cpp -o /tmp/firestar-project-tests
/tmp/firestar-project-tests
```

From a Visual Studio x64 Native Tools Command Prompt:

```bat
cl /nologo /std:c++20 /EHsc /W4 /Isrc /Isrc\repak\thirdparty tests\ProjectDocumentTests.cpp src\editor\ProjectDocument.cpp /Fe:%TEMP%\firestar-project-tests.exe
%TEMP%\firestar-project-tests.exe
```

The tests create and remove their own temporary directory. Failures return a
nonzero exit code in both debug and release builds. The GitHub Actions workflow
runs this focused suite on Linux and Windows; it does not build or exercise the
complete Firestar application.
