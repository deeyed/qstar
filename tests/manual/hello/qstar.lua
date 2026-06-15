qstar.project {
  name = "hello",
  version = "0.1.0",
  root = ".",
}

qstar.subdir("src/foo")

qstar.custom_target "version" {
    inputs = {
        "VERSION",
    },
    outputs = {
        qstar.output("generated/version.c"),
    },
    command = qstar.cli {
        "version-gen",
        "--in", "VERSION",
        "--out", qstar.output(0),
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
    lang = {
        c = {
            public_headers = {
                "include/hello/api.h",
            },
            include_dirs = {
                "include",
                "generated",
            },
        },
    },
}
