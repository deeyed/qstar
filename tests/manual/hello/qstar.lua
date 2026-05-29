qstar.subdir("src/foo")

qstar.genrule "version" {
    tool = "version-gen",
    inputs = {
        "VERSION",
    },
    outputs = {
        qstar.output("generated/version.c"),
    },
    args = {
        "--in", "VERSION",
        "--out", qstar.output("generated/version.c"),
    },
}

qstar.exe "app" {
    deps = {
        "//src/foo:foo",
    },
    sources = {
        qstar.output("generated/version.c"),
        "src/main.c",
    },
    public_headers = {
        "include/hello/api.h",
    },
    include_dirs = {
        "include",
        "generated",
    },
    toolchain = "host",
    stdlib = "system",
}
