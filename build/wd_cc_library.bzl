"""wd_cc_library definition"""

load(":build/wd_copts.bzl", "wd_copts")

def wd_cc_library(
        name,
        copts = [],
        strip_include_prefix = "/src",
        **kwargs):
    """Wrapper for cc_library that sets common attributes
    """
    native.cc_library(
        name = name,
        copts = copts + wd_copts(),
        strip_include_prefix = strip_include_prefix,
        **kwargs
    )
