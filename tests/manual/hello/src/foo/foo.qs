qstar.staticlib "foo" {
    sources = {
        "src/foo/foo.c",
    },
    private_headers = {
        "src/foo/foo_internal.h",
    },
    toolchain = "host",
    stdlib = "system",
}
