qstar.subdir("src/foo")

qstar.custom_target "version" {
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

qstar.executable "app" {
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
    lang = {
        c = {
            include_dirs = {
                "include",
                "generated",
            },
        },
    },
    toolchain = "host",
    stdlib = "system",
}
