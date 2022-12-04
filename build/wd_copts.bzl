"""wd_copts definition"""

def wd_copts():
    """Common OS copts for all cc_binary, cc_library, and cc_test targets"""
    return select({
        "@platforms//os:windows": [
            "/std:c++20",

            # https://learn.microsoft.com/en-us/cpp/build/reference/zc-conformance?view=msvc-170
            "/Zc:__cplusplus",
            # Enable updated `__cplusplus` macro.
            # See https://learn.microsoft.com/en-us/cpp/build/reference/zc-cplusplus?view=msvc-170.
            "/Zc:preprocessor",
            # Enable preprocessor conformance mode. JSG uses lots of variadic macros that break without this.
            # See https://learn.microsoft.com/en-us/cpp/build/reference/zc-preprocessor?view=msvc-170.
            "/Zc:lambda",

            "/Zc:rvalueCast",


            "/TP",
            # MSVC doesn't recognize .c++ files as C++ source files by defualt,
            # so explicitly treat all input source files as C++.
            # See https://learn.microsoft.com/en-us/cpp/build/reference/tc-tp-tc-tp-specify-source-file-type?view=msvc-170.

            "/await",
            "/wo4503",

            "/DWINDOWS_LEAN_AND_MEAN"
        ],
        "//conditions:default": [
            "-std=c++20",
            "-fcoroutines-ts",
            "-stdlib=libc++",
            "-Wall",
            "-Wextra",
            "-Wno-strict-aliasing",
            "-Wno-sign-compare",
            "-Wno-unused-parameter",
            "-Wno-missing-field-initializers",
            "-Wno-ignored-qualifiers",
        ],
    })
