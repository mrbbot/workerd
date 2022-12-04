"""wd_cc_binary definition"""

load(":build/wd_copts.bzl", "wd_copts")

def wd_cc_binary(
        name,
        copts = [],
        visibility = None,
        **kwargs):
    """Wrapper for cc_binary that sets common attributes
    """
    native.cc_binary(
        name = name,
        copts = copts + wd_copts(),
        visibility = visibility,
        **kwargs
    )
