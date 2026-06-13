qstar.project {
  name = "object-artifact-bridge",
  version = "0.1.0",
  root = ".",
  build_dir = "build/qstar",
  generated_dir = "build/qstar/generated",
}

qstar.custom_target "objc_object" {
  inputs = {
    "src/AppDelegate.m",
  },
  outputs = {
    qstar.output("build/qstar/generated/objc/AppDelegate.o", {
      format = "object",
    }),
  },
  command = qstar.cli {
    "tools/fake-objc-compile.sh",
    qstar.input(0),
    qstar.output(0),
  },
  description = qstar.status("Building Objective-C object AppDelegate.o"),
}

qstar.staticlib "objc_static" {
  sources = {
    qstar.output("build/qstar/generated/objc/AppDelegate.o"),
  },
}

qstar.sharedlib "objc_plugin" {
  sources = {
    "src/plugin.c",
    qstar.output("build/qstar/generated/objc/AppDelegate.o"),
  },
}

qstar.executable "app" {
  sources = {
    "src/main.c",
    qstar.output("build/qstar/generated/objc/AppDelegate.o"),
  },
}

qstar.group "all" {
  deps = {
    "//:app",
    "//:objc_static",
    "//:objc_plugin",
  },
}
